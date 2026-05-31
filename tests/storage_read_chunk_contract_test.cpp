#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"

namespace
{
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
            .expected_checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    std::shared_ptr<storedemo::ShardedChunkIndex> MakeSharedIndex()
    {
        return std::make_shared<storedemo::ShardedChunkIndex>();
    }

    std::filesystem::path ResolveFinalPathOrThrow(const std::filesystem::path &data_root,
                                                  const storedemo::ChunkId &chunk_id)
    {
        storedemo::ChunkPathLayout layout;
        std::string error_detail;
        const auto layout_status = storedemo::BuildChunkPathLayout(chunk_id,
                                                                   "probe",
                                                                   &layout,
                                                                   &error_detail);
        if (layout_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk layout: " + error_detail);
        }

        std::filesystem::path final_path;
        const auto resolve_status = storedemo::ResolveDurablePathUnderRoot(data_root,
                                                                           layout.final_relative_path,
                                                                           &final_path,
                                                                           &error_detail);
        if (resolve_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to resolve final path: " + error_detail);
        }

        return final_path;
    }

    std::filesystem::path ResolveStagingPathOrThrow(const std::filesystem::path &data_root,
                                                    const storedemo::ChunkId &chunk_id,
                                                    const std::string_view token)
    {
        storedemo::ChunkPathLayout layout;
        std::string error_detail;
        const auto layout_status = storedemo::BuildChunkPathLayout(chunk_id,
                                                                   token,
                                                                   &layout,
                                                                   &error_detail);
        if (layout_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk layout: " + error_detail);
        }

        std::filesystem::path staging_path;
        const auto resolve_status = storedemo::ResolveDurablePathUnderRoot(data_root,
                                                                           layout.staging_relative_path,
                                                                           &staging_path,
                                                                           &error_detail);
        if (resolve_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to resolve staging path: " + error_detail);
        }

        return staging_path;
    }

    storedemo::ChunkIndexEntry FindIndexEntryOrThrow(storedemo::ChunkIndex &index,
                                                     const storedemo::ChunkId &chunk_id)
    {
        const auto find_response = index.Find(chunk_id);
        if (!find_response.ok())
        {
            throw std::runtime_error("failed to find chunk index entry: " +
                                     find_response.error_detail);
        }
        return find_response.entry;
    }

    void UpdateIndexStateOrThrow(storedemo::ChunkIndex &index,
                                 const storedemo::ChunkId &chunk_id,
                                 const storedemo::ChunkState state)
    {
        auto entry = FindIndexEntryOrThrow(index, chunk_id);
        entry.state = state;
        ++entry.updated_at;

        const auto update_response = index.Update(entry);
        if (!update_response.ok())
        {
            throw std::runtime_error("failed to update chunk index entry: " +
                                     update_response.error_detail);
        }
    }

    class ReadChunkContractAdapter
    {
    public:
        explicit ReadChunkContractAdapter(storedemo::ChunkStore &store)
            : store_(store)
        {
        }

        storedemo::ReadChunkResponse ReadChunk(
            const storedemo::ReadChunkRequest &request) const
        {
            return store_.ReadChunk(request);
        }

    private:
        storedemo::ChunkStore &store_;
    };

    storedemo::StorageNodeStatusCode MapGrpcStatusCodeForReadContract(
        const grpc::StatusCode code)
    {
        switch (code)
        {
        case grpc::StatusCode::OK:
            return storedemo::StorageNodeStatusCode::kOk;
        case grpc::StatusCode::NOT_FOUND:
            return storedemo::StorageNodeStatusCode::kNotFound;
        case grpc::StatusCode::INVALID_ARGUMENT:
            return storedemo::StorageNodeStatusCode::kInvalidArgument;
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            return storedemo::StorageNodeStatusCode::kTimeout;
        case grpc::StatusCode::UNAVAILABLE:
            return storedemo::StorageNodeStatusCode::kNodeUnavailable;
        case grpc::StatusCode::INTERNAL:
        case grpc::StatusCode::UNKNOWN:
        case grpc::StatusCode::DATA_LOSS:
        default:
            return storedemo::StorageNodeStatusCode::kIoError;
        }
    }

    struct FakeReadRpcResult
    {
        grpc::Status grpc_status{grpc::Status::OK};
        storedemo::ReadChunkResponse response;
    };

    class ReadChunkClientContractAdapter
    {
    public:
        using Handler =
            std::function<FakeReadRpcResult(const storedemo::ReadChunkRequest &)>;

        explicit ReadChunkClientContractAdapter(Handler handler)
            : handler_(std::move(handler))
        {
        }

        storedemo::ReadChunkResponse ReadChunk(
            const storedemo::ReadChunkRequest &request) const
        {
            const auto result = handler_(request);
            if (result.grpc_status.ok())
            {
                return result.response;
            }

            storedemo::ReadChunkResponse response;
            response.status =
                MapGrpcStatusCodeForReadContract(result.grpc_status.error_code());
            response.error_detail = result.grpc_status.error_message();
            return response;
        }

    private:
        Handler handler_;
    };

    class StorageReadChunkContractTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStore MakeStore(
            const std::filesystem::path &root,
            const std::size_t node_index,
            const std::shared_ptr<storedemo::ShardedChunkIndex> &index = MakeSharedIndex())
        {
            return storedemo::LocalDiskChunkStore(
                storedemo::LocalDiskChunkStoreConfig{
                    .data_dir = root,
                    .node_id = storedemo::test::MakeStorageNodeIdFixture(node_index),
                    .chunk_index = index});
        }
    };

    TEST_F(StorageReadChunkContractTest, FullReadReturnsFixturePayloadChecksumAndSize)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T041 real local ReadChunk contract validation is Linux-primary in this environment";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_chunk_contract_full");
        auto shared_index = MakeSharedIndex();
        auto store = MakeStore(temp_dir.Path("store"), 41, shared_index);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");
        ASSERT_FALSE(fixture.payload.empty());

        const auto identity = MakeStoreIdentityOrThrow("obj-t041-full", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t041-full"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ReadChunkContractAdapter adapter(store);
        auto request = MakeReadRequest(identity.chunk_id, "read-t041-full");
        request.expected_checksum = storedemo::test::ComputeStoreChecksumOrThrow(
            fixture.payload);
        request.verify_checksum = true;

        const auto response = adapter.ReadChunk(request);
        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);
        EXPECT_EQ(response.metadata.size, fixture.payload.size());
        EXPECT_EQ(response.metadata.checksum.value, request.expected_checksum.value);
        EXPECT_EQ(response.actual_checksum.value, request.expected_checksum.value);
        EXPECT_EQ(response.payload, fixture.payload);
        EXPECT_TRUE(response.verified);
#endif
    }

    TEST_F(StorageReadChunkContractTest, RangeReadReturnsExplicitBoundaryInCurrentStage)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T041 real local ReadChunk contract validation is Linux-primary in this environment";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_chunk_contract_range");
        auto store = MakeStore(temp_dir.Path("store"), 42);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        const auto identity = MakeStoreIdentityOrThrow("obj-t041-range", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t041-range"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ReadChunkContractAdapter adapter(store);
        auto request = MakeReadRequest(identity.chunk_id, "read-t041-range");
        request.range = storedemo::ChunkReadRange{.offset = 3, .length = 17};
        const auto response = adapter.ReadChunk(request);

        EXPECT_TRUE(response.status == storedemo::StorageNodeStatusCode::kUnsupported ||
                    response.status ==
                        storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_FALSE(response.ok());
        EXPECT_TRUE(response.payload.empty());
#endif
    }

    TEST_F(StorageReadChunkContractTest, ExpectedChecksumMismatchReturnsChecksumMismatch)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T041 real local ReadChunk contract validation is Linux-primary in this environment";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_chunk_contract_checksum");
        auto store = MakeStore(temp_dir.Path("store"), 43);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        const auto identity = MakeStoreIdentityOrThrow("obj-t041-checksum", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t041-checksum"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ReadChunkContractAdapter adapter(store);
        auto request = MakeReadRequest(identity.chunk_id, "read-t041-checksum");
        request.expected_checksum =
            storedemo::test::ComputeStoreChecksumOrThrow("wrong-payload");
        request.verify_checksum = true;

        const auto response = adapter.ReadChunk(request);
        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(response.ok());
        EXPECT_TRUE(response.payload.empty());
#endif
    }

    TEST_F(StorageReadChunkContractTest, ReadChunkRejectsEveryNonLiveState)
    {
        const struct NonLiveCase
        {
            const char *name;
            storedemo::ChunkState state;
            storedemo::StorageNodeStatusCode expected_status;
        } cases[] = {
            {"quarantined", storedemo::ChunkState::kQuarantined,
             storedemo::StorageNodeStatusCode::kCorrupted},
            {"corrupted", storedemo::ChunkState::kCorrupted,
             storedemo::StorageNodeStatusCode::kCorrupted},
            {"deleted", storedemo::ChunkState::kDeleted,
             storedemo::StorageNodeStatusCode::kNotFound},
            {"staging", storedemo::ChunkState::kStaging,
             storedemo::StorageNodeStatusCode::kConflict},
        };

        for (const auto &test_case : cases)
        {
            SCOPED_TRACE(test_case.name);

            storedemo::test::ScopedStoreTestDir temp_dir(
                std::string("storage_read_chunk_contract_") + test_case.name);
            auto shared_index = MakeSharedIndex();
            auto store =
                MakeStore(temp_dir.Path(test_case.name), 44, shared_index);
            ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

            const auto payload = storedemo::test::MakeChunkPayload(
                96,
                std::string("t041-") + test_case.name);
            const auto identity = MakeStoreIdentityOrThrow(
                std::string("obj-t041-") + test_case.name,
                1,
                0,
                0);
            const auto write = store.WriteChunk(
                MakeWriteRequest(identity,
                                 payload,
                                 std::string("write-t041-") + test_case.name));
            ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
                << write.error_detail;

            ASSERT_NO_THROW(UpdateIndexStateOrThrow(*shared_index,
                                                    identity.chunk_id,
                                                    test_case.state));

            ReadChunkContractAdapter adapter(store);
            const auto response = adapter.ReadChunk(
                MakeReadRequest(identity.chunk_id,
                                std::string("read-t041-") + test_case.name));
            EXPECT_EQ(response.status, test_case.expected_status);
            EXPECT_FALSE(response.ok());
            EXPECT_TRUE(response.payload.empty());
        }
    }

    TEST_F(StorageReadChunkContractTest, ReadChunkDoesNotFallBackToStagingWhenLiveFileIsMissing)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T041 real local ReadChunk contract validation is Linux-primary in this environment";
#else
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_read_chunk_contract_staging_fallback");
        auto shared_index = MakeSharedIndex();
        auto store = MakeStore(temp_dir.Path("store"), 45, shared_index);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        const auto identity = MakeStoreIdentityOrThrow("obj-t041-live-only", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t041-live-only"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                        identity.chunk_id);
        ASSERT_TRUE(std::filesystem::remove(final_path));

        const auto staging_path = ResolveStagingPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id,
                                                            "manual-staging");
        std::filesystem::create_directories(staging_path.parent_path());
        {
            std::ofstream output(staging_path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open());
            output << fixture.payload;
        }

        ReadChunkContractAdapter adapter(store);
        const auto response = adapter.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t041-live-only"));
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_FALSE(response.ok());
        EXPECT_TRUE(response.payload.empty());
#endif
    }

    TEST_F(StorageReadChunkContractTest, MissingChunkReturnsNotFound)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_chunk_contract_missing");
        auto store = MakeStore(temp_dir.Path("store"), 46);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t041-missing", 1, 0, 0);
        ReadChunkContractAdapter adapter(store);
        const auto response = adapter.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t041-missing"));
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_FALSE(response.ok());
    }

    TEST_F(StorageReadChunkContractTest, ReadChunkDoesNotDecideObjectCommittedVisibility)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T041 real local ReadChunk contract validation is Linux-primary in this environment";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_chunk_contract_pending");
        auto store = MakeStore(temp_dir.Path("store"), 47);
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand("bucket-t041",
                                                                "create-bucket-t041"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        const auto identity = MakeStoreIdentityOrThrow("obj-t041-pending", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t041",
                            "objects/test_file.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t041",
                            fixture.payload.size(),
                            "etag-t041"))
                        .Ok);

        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t041-pending"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t041", .object_key = "objects/test_file.deb"});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        ReadChunkContractAdapter adapter(store);
        auto request = MakeReadRequest(identity.chunk_id, "read-t041-pending");
        request.expected_checksum = storedemo::test::ComputeStoreChecksumOrThrow(
            fixture.payload);
        request.verify_checksum = true;
        const auto response = adapter.ReadChunk(request);

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_EQ(response.payload, fixture.payload);

        const auto head_after = machine.HeadObject(
            {.bucket = "bucket-t041", .object_key = "objects/test_file.deb"});
        ASSERT_EQ(head_after.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head_after.record.has_value());
#endif
    }

    TEST_F(StorageReadChunkContractTest, FutureClientTransportStatusMappingsStayStable)
    {
        const struct MappingCase
        {
            const char *name;
            grpc::StatusCode grpc_code;
            storedemo::StorageNodeStatusCode expected_status;
        } cases[] = {
            {"not_found", grpc::StatusCode::NOT_FOUND,
             storedemo::StorageNodeStatusCode::kNotFound},
            {"invalid_argument", grpc::StatusCode::INVALID_ARGUMENT,
             storedemo::StorageNodeStatusCode::kInvalidArgument},
            {"timeout", grpc::StatusCode::DEADLINE_EXCEEDED,
             storedemo::StorageNodeStatusCode::kTimeout},
            {"unavailable", grpc::StatusCode::UNAVAILABLE,
             storedemo::StorageNodeStatusCode::kNodeUnavailable},
            {"io_error", grpc::StatusCode::INTERNAL,
             storedemo::StorageNodeStatusCode::kIoError},
        };

        for (const auto &test_case : cases)
        {
            SCOPED_TRACE(test_case.name);
            ReadChunkClientContractAdapter adapter(
                [test_case](const storedemo::ReadChunkRequest &) -> FakeReadRpcResult
                {
                    FakeReadRpcResult result;
                    result.grpc_status = grpc::Status(test_case.grpc_code,
                                                      std::string("mapped-") +
                                                          test_case.name);
                    return result;
                });

            const auto response = adapter.ReadChunk(
                MakeReadRequest("fake-chunk-id", std::string("rpc-") + test_case.name));
            EXPECT_EQ(response.status, test_case.expected_status);
            EXPECT_EQ(response.error_detail,
                      std::string("mapped-") + test_case.name);
        }
    }
}
