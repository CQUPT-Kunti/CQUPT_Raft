#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    struct FixtureBinaryPayload
    {
        std::string payload;
        std::filesystem::path source_path;
        bool used_repo_fixture{false};
    };

    struct BatchDeleteChunkItemResult
    {
        storedemo::DeleteChunkRequest request;
        storedemo::DeleteChunkResponse response;
        bool retryable{false};
    };

    struct BatchDeleteChunkContractResult
    {
        std::vector<BatchDeleteChunkItemResult> items;
    };

    std::filesystem::path RepoRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
    }

    FixtureBinaryPayload LoadFixtureBinaryPayload()
    {
        const std::filesystem::path primary_path =
            RepoRoot() / "tests" / "test_file" / "test_file.deb";
        const std::filesystem::path fallback_path =
            RepoRoot() / "test" / "test_file" / "test_file.deb";

        for (const auto &candidate : {primary_path, fallback_path})
        {
            if (!std::filesystem::exists(candidate))
            {
                continue;
            }

            std::ifstream input(candidate, std::ios::binary);
            if (!input.is_open())
            {
                throw std::runtime_error("failed to open binary fixture: " +
                                         candidate.string());
            }

            return FixtureBinaryPayload{
                .payload = std::string(std::istreambuf_iterator<char>(input),
                                       std::istreambuf_iterator<char>()),
                .source_path = candidate,
                .used_repo_fixture = true};
        }

        throw std::runtime_error("missing delete chunk contract binary fixture");
    }

    storedemo::ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::ChunkIdentity MakeIdentityOrThrow(const std::string_view object_id,
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
            .expected_checksum = ComputeChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storedemo::DeleteChunkRequest MakeDeleteRequest(const storedemo::ChunkId &chunk_id,
                                                    const std::string &request_id)
    {
        storedemo::DeleteChunkRequest request;
        request.request_id = request_id;
        request.chunk_id = chunk_id;
        request.reason = "contract test";
        request.metadata_boundary = "test-only-contract";
        return request;
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

    raftdemo::MetadataCommand MakeCreateObjectCommandWithSize(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        const std::uint64_t create_time = 1712100001)
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

    raftdemo::MetadataCommand MakeCommitObjectCommandWithChunks(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        std::vector<raftdemo::ChunkRef> chunks,
        const std::uint64_t commit_time = 1712100002)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            size,
            etag,
            std::move(chunks),
            commit_time};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCommitObject,
            bucket,
            object_key,
            "accepted",
            0,
            commit_time,
            std::nullopt};
        return command;
    }

    raftdemo::ChunkRef MakeChunkRefFromMetadata(const storedemo::ChunkMetadata &metadata)
    {
        return raftdemo::ChunkRef{
            .chunk_id = metadata.identity.chunk_id,
            .offset = metadata.identity.offset,
            .size = metadata.size,
            .replica_nodes = {metadata.node_id},
            .checksum = metadata.checksum.value};
    }

    class DeleteChunkContractAdapter
    {
    public:
        explicit DeleteChunkContractAdapter(std::shared_ptr<storedemo::ChunkStore> chunk_store)
            : chunk_store_(std::move(chunk_store))
        {
            if (chunk_store_ == nullptr)
            {
                throw std::invalid_argument("chunk_store must not be null");
            }
        }

        void ForceDeleteResponse(const storedemo::ChunkId &chunk_id,
                                 storedemo::DeleteChunkResponse response)
        {
            forced_delete_responses_[chunk_id] = std::move(response);
        }

        void ClearForcedDeleteResponse(const storedemo::ChunkId &chunk_id)
        {
            forced_delete_responses_.erase(chunk_id);
        }

        storedemo::DeleteChunkResponse DeleteChunk(
            const storedemo::DeleteChunkRequest &request) const
        {
            const auto forced = forced_delete_responses_.find(request.chunk_id);
            if (forced == forced_delete_responses_.end())
            {
                return chunk_store_->DeleteChunk(request);
            }

            auto response = forced->second;
            if (response.metadata.identity.chunk_id.empty())
            {
                response.metadata.identity.chunk_id = request.chunk_id;
            }
            return response;
        }

        BatchDeleteChunkContractResult BatchDeleteChunks(
            const std::vector<storedemo::DeleteChunkRequest> &requests) const
        {
            BatchDeleteChunkContractResult result;
            result.items.reserve(requests.size());
            for (const auto &request : requests)
            {
                auto response = DeleteChunk(request);
                result.items.push_back(BatchDeleteChunkItemResult{
                    .request = request,
                    .response = response,
                    .retryable = response.status != storedemo::StorageNodeStatusCode::kOk &&
                                 storedemo::IsRetriableStatus(response.status)});
            }
            return result;
        }

    private:
        std::shared_ptr<storedemo::ChunkStore> chunk_store_;
        std::unordered_map<storedemo::ChunkId, storedemo::DeleteChunkResponse>
            forced_delete_responses_;
    };

    class StorageDeleteChunkContractTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStoreConfig MakeConfig(
            const std::filesystem::path &root,
            const storedemo::StorageNodeId &node_id,
            std::shared_ptr<storedemo::ChunkIndex> chunk_index = {})
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = root,
                .node_id = node_id,
                .chunk_index = std::move(chunk_index)};
        }
    };

    TEST_F(StorageDeleteChunkContractTest,
           DeleteChunkRemovesLiveChunkWithoutChangingMetadataVisibility)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T050 delete chunk contract is currently validated on Linux";
#else
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_chunk_contract_live");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeConfig(temp_dir.Path("node-data"),
                       storedemo::test::MakeStorageNodeIdFixture(50)));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        DeleteChunkContractAdapter adapter(store);
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t050-live",
                            "create-bucket-t050-live"))
                        .Ok);

        const auto identity = MakeIdentityOrThrow("obj-t050-live", 1, 0, 0);
        const auto write_response = store->WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t050-live"));
        ASSERT_EQ(write_response.status, storedemo::StorageNodeStatusCode::kOk)
            << write_response.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t050-live",
                                                        "objects/test_file.deb",
                                                        identity.object_id,
                                                        "create-object-t050-live",
                                                        fixture.payload.size(),
                                                        "etag-t050-live"))
                        .Ok);
        std::vector<raftdemo::ChunkRef> manifest_chunks;
        manifest_chunks.push_back(MakeChunkRefFromMetadata(write_response.metadata));
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCommitObjectCommandWithChunks("bucket-t050-live",
                                                          "objects/test_file.deb",
                                                          identity.object_id,
                                                          "commit-object-t050-live",
                                                          fixture.payload.size(),
                                                          "etag-t050-live",
                                                          std::move(manifest_chunks)))
                        .Ok);

        const auto delete_response =
            adapter.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "delete-t050-live"));
        ASSERT_EQ(delete_response.status, storedemo::StorageNodeStatusCode::kOk)
            << delete_response.error_detail;
        EXPECT_TRUE(delete_response.deleted);
        EXPECT_FALSE(delete_response.already_missing);

        const auto read_after_delete = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t050-live-after-delete"));
        EXPECT_NE(read_after_delete.status, storedemo::StorageNodeStatusCode::kOk);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t050-live", .object_key = "objects/test_file.deb"});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());

        const auto listed = machine.ListObjects(
            {.bucket = "bucket-t050-live", .prefix = "objects/"});
        ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(listed.records.size(), 1U);
        EXPECT_EQ(listed.records.front().object_key, "objects/test_file.deb");
#endif
    }

    TEST_F(StorageDeleteChunkContractTest,
           DeleteChunkHasExplicitIdempotentAndCorruptionStateSemantics)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T050 delete chunk contract is currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_chunk_contract_states");
        const auto shared_index = std::make_shared<storedemo::ShardedChunkIndex>();
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeConfig(temp_dir.Path("node-data"),
                       storedemo::test::MakeStorageNodeIdFixture(51),
                       shared_index));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        DeleteChunkContractAdapter adapter(store);

        const auto missing_identity = MakeIdentityOrThrow("obj-t050-missing", 1, 0, 0);
        const auto missing_response = adapter.DeleteChunk(
            MakeDeleteRequest(missing_identity.chunk_id, "delete-t050-missing"));
        EXPECT_EQ(missing_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(missing_response.already_missing);
        EXPECT_EQ(missing_response.metadata.state, storedemo::ChunkState::kMissing);

        const auto deleted_identity = MakeIdentityOrThrow("obj-t050-deleted", 1, 0, 0);
        const auto deleted_payload = storedemo::test::MakeChunkPayload(64, "t050-deleted");
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(deleted_identity,
                                       deleted_payload,
                                       "write-t050-deleted"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(adapter.DeleteChunk(
                      MakeDeleteRequest(deleted_identity.chunk_id, "delete-t050-deleted-first"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        const auto deleted_again = adapter.DeleteChunk(
            MakeDeleteRequest(deleted_identity.chunk_id, "delete-t050-deleted-second"));
        EXPECT_EQ(deleted_again.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(deleted_again.already_missing);
        EXPECT_EQ(deleted_again.metadata.state, storedemo::ChunkState::kDeleted);

        const auto quarantined_identity =
            MakeIdentityOrThrow("obj-t050-quarantined", 1, 0, 0);
        const auto quarantined_payload =
            storedemo::test::MakeChunkPayload(48, "t050-quarantined");
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(quarantined_identity,
                                       quarantined_payload,
                                       "write-t050-quarantined"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        UpdateIndexStateOrThrow(*shared_index,
                                quarantined_identity.chunk_id,
                                storedemo::ChunkState::kQuarantined);
        const auto quarantined_delete = adapter.DeleteChunk(
            MakeDeleteRequest(quarantined_identity.chunk_id, "delete-t050-quarantined"));
        EXPECT_EQ(quarantined_delete.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(quarantined_delete.deleted);
        EXPECT_EQ(store->ReadChunk(
                      MakeReadRequest(quarantined_identity.chunk_id,
                                      "read-t050-quarantined-after-delete"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);

        const auto corrupted_identity =
            MakeIdentityOrThrow("obj-t050-corrupted", 1, 0, 0);
        const auto corrupted_payload =
            storedemo::test::MakeChunkPayload(48, "t050-corrupted");
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(corrupted_identity,
                                       corrupted_payload,
                                       "write-t050-corrupted"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        UpdateIndexStateOrThrow(*shared_index,
                                corrupted_identity.chunk_id,
                                storedemo::ChunkState::kCorrupted);
        const auto corrupted_delete = adapter.DeleteChunk(
            MakeDeleteRequest(corrupted_identity.chunk_id, "delete-t050-corrupted"));
        EXPECT_EQ(corrupted_delete.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(corrupted_delete.deleted);
        EXPECT_EQ(store->ReadChunk(
                      MakeReadRequest(corrupted_identity.chunk_id,
                                      "read-t050-corrupted-after-delete"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
#endif
    }

    TEST_F(StorageDeleteChunkContractTest,
           DeleteChunkRejectsChecksumMismatchWithoutRemovingChunk)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T050 delete chunk contract is currently validated on Linux";
#else
        const auto fixture = LoadFixtureBinaryPayload();
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_chunk_contract_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeConfig(temp_dir.Path("node-data"),
                       storedemo::test::MakeStorageNodeIdFixture(52)));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        DeleteChunkContractAdapter adapter(store);
        const auto identity = MakeIdentityOrThrow("obj-t050-checksum", 1, 0, 0);
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(identity,
                                       fixture.payload,
                                       "write-t050-checksum"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto delete_request = MakeDeleteRequest(identity.chunk_id, "delete-t050-checksum");
        delete_request.expected_checksum = ComputeChecksumOrThrow("different-payload");
        const auto delete_response = adapter.DeleteChunk(delete_request);
        EXPECT_EQ(delete_response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(delete_response.ok());

        const auto read_response = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t050-checksum-after-failure"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
#endif
    }

    TEST_F(StorageDeleteChunkContractTest,
           BatchDeleteChunksReturnsIndependentResultsAndRetrySignals)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T050 delete chunk contract is currently validated on Linux";
#else
        const auto fixture = LoadFixtureBinaryPayload();
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_chunk_contract_batch");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeConfig(temp_dir.Path("node-data"),
                       storedemo::test::MakeStorageNodeIdFixture(53)));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        DeleteChunkContractAdapter adapter(store);

        const auto success_identity = MakeIdentityOrThrow("obj-t050-batch-success", 1, 0, 0);
        const auto deleted_identity = MakeIdentityOrThrow("obj-t050-batch-deleted", 1, 0, 0);
        const auto retry_identity = MakeIdentityOrThrow("obj-t050-batch-retry", 1, 0, 0);
        const auto nonretry_identity = MakeIdentityOrThrow("obj-t050-batch-nonretry", 1, 0, 0);
        const auto missing_identity = MakeIdentityOrThrow("obj-t050-batch-missing", 1, 0, 0);

        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(success_identity,
                                       fixture.payload,
                                       "write-t050-batch-success"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(deleted_identity,
                                       fixture.payload,
                                       "write-t050-batch-deleted"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(retry_identity,
                                       fixture.payload,
                                       "write-t050-batch-retry"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      MakeWriteRequest(nonretry_identity,
                                       fixture.payload,
                                       "write-t050-batch-nonretry"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(adapter.DeleteChunk(
                      MakeDeleteRequest(deleted_identity.chunk_id,
                                        "delete-t050-batch-deleted-prime"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        storedemo::DeleteChunkResponse retryable_response;
        retryable_response.status = storedemo::StorageNodeStatusCode::kTimeout;
        retryable_response.error_detail = "forced retryable timeout";
        retryable_response.retry_after_ms = 25;
        adapter.ForceDeleteResponse(retry_identity.chunk_id, retryable_response);

        storedemo::DeleteChunkResponse nonretryable_response;
        nonretryable_response.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
        nonretryable_response.error_detail = "forced non-retryable invalid argument";
        adapter.ForceDeleteResponse(nonretry_identity.chunk_id, nonretryable_response);

        std::vector<storedemo::DeleteChunkRequest> batch_requests;
        batch_requests.push_back(
            MakeDeleteRequest(success_identity.chunk_id, "batch-t050-success"));
        batch_requests.push_back(
            MakeDeleteRequest(missing_identity.chunk_id, "batch-t050-missing"));
        batch_requests.push_back(
            MakeDeleteRequest(deleted_identity.chunk_id, "batch-t050-deleted"));
        batch_requests.push_back(
            MakeDeleteRequest(retry_identity.chunk_id, "batch-t050-retry"));
        batch_requests.push_back(
            MakeDeleteRequest(nonretry_identity.chunk_id, "batch-t050-nonretry"));

        const auto batch_result = adapter.BatchDeleteChunks(batch_requests);
        ASSERT_EQ(batch_result.items.size(), 5U);

        EXPECT_EQ(batch_result.items[0].response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(batch_result.items[0].response.deleted);
        EXPECT_FALSE(batch_result.items[0].retryable);

        EXPECT_EQ(batch_result.items[1].response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(batch_result.items[1].response.already_missing);
        EXPECT_FALSE(batch_result.items[1].retryable);

        EXPECT_EQ(batch_result.items[2].response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(batch_result.items[2].response.already_missing);
        EXPECT_FALSE(batch_result.items[2].retryable);

        EXPECT_EQ(batch_result.items[3].response.status,
                  storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_TRUE(batch_result.items[3].retryable);
        EXPECT_EQ(batch_result.items[3].response.retry_after_ms, 25U);

        EXPECT_EQ(batch_result.items[4].response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_FALSE(batch_result.items[4].retryable);

        EXPECT_EQ(store->ReadChunk(
                      MakeReadRequest(success_identity.chunk_id,
                                      "read-t050-batch-success-after-delete"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
        ASSERT_EQ(store->ReadChunk(
                      MakeReadRequest(retry_identity.chunk_id,
                                      "read-t050-batch-retry-still-live"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->ReadChunk(
                      MakeReadRequest(nonretry_identity.chunk_id,
                                      "read-t050-batch-nonretry-still-live"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        adapter.ClearForcedDeleteResponse(retry_identity.chunk_id);
        const auto retry_batch = adapter.BatchDeleteChunks(
            {MakeDeleteRequest(retry_identity.chunk_id, "batch-t050-retry-second")});
        ASSERT_EQ(retry_batch.items.size(), 1U);
        EXPECT_EQ(retry_batch.items.front().response.status,
                  storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(retry_batch.items.front().response.deleted);
        EXPECT_FALSE(retry_batch.items.front().retryable);
        EXPECT_EQ(store->ReadChunk(
                      MakeReadRequest(retry_identity.chunk_id,
                                      "read-t050-batch-retry-after-success"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);

        const auto success_again = adapter.DeleteChunk(
            MakeDeleteRequest(success_identity.chunk_id, "delete-t050-batch-success-again"));
        EXPECT_EQ(success_again.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(success_again.already_missing);

        const auto missing_again = adapter.DeleteChunk(
            MakeDeleteRequest(missing_identity.chunk_id, "delete-t050-batch-missing-again"));
        EXPECT_EQ(missing_again.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(missing_again.already_missing);

        const auto deleted_again = adapter.DeleteChunk(
            MakeDeleteRequest(deleted_identity.chunk_id, "delete-t050-batch-deleted-again"));
        EXPECT_EQ(deleted_again.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(deleted_again.already_missing);
#endif
    }
} // namespace
