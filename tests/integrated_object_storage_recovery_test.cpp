#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "raft/state_machine/metadata_state_machine.h"
#include "store/common/store_types.h"
#include "store/placement/replica_policy.h"
#include "store/upload/upload_coordinator.h"
#include "support/storage_upload_test_utils.h"

namespace
{
    std::string MakeSyntheticUploadPayload()
    {
        std::string payload;
        payload.reserve(4096);
        for (std::size_t index = 0; index < 4096; ++index)
        {
            payload.push_back(static_cast<char>((index * 17 + 29) % 251));
        }
        return payload;
    }

    storedemo::StorageNodePlacementCandidate MakeCandidate(
        const std::string &node_id,
        const std::uint64_t available_capacity_bytes,
        const storedemo::StorageNodeHealth health)
    {
        storedemo::StorageNodePlacementCandidate candidate;
        candidate.node_id = node_id;
        candidate.endpoint = "test-endpoint-" + node_id;
        candidate.health = health;
        candidate.disk_pressure = storedemo::StorageNodeDiskPressure::kLow;
        candidate.total_capacity_bytes = available_capacity_bytes + 8192;
        candidate.used_capacity_bytes = 8192;
        candidate.available_capacity_bytes = available_capacity_bytes;
        return candidate;
    }

    storedemo::ReplicaPolicy MakeReplicaPolicy(const std::size_t replica_count,
                                               const std::size_t minimum_successful_writes,
                                               const std::uint64_t reserve_capacity_bytes)
    {
        storedemo::ReplicaPolicy policy;
        policy.replica_count = replica_count;
        policy.minimum_successful_writes = minimum_successful_writes;
        policy.avoid_same_node = true;
        policy.reserve_capacity_bytes = reserve_capacity_bytes;
        return policy;
    }

    storedemo::UploadCoordinatorRequest MakeNoHealthyStorageRequest(
        const std::string &payload)
    {
        storedemo::UploadCoordinatorRequest request;
        request.request_id = "upload-t079-no-healthy";
        request.bucket = "bucket-t079";
        request.object_key = "objects/no-healthy-capacity.bin";
        request.object_id = "obj-t079";
        request.version = 1;
        request.replica_policy = MakeReplicaPolicy(2, 2, 128);
        request.client_time_unix_ms = 1713007900;
        request.candidates = {
            MakeCandidate("store-unavailable",
                          128ULL * 1024ULL,
                          storedemo::StorageNodeHealth::kUnavailable),
            MakeCandidate("store-readonly",
                          128ULL * 1024ULL,
                          storedemo::StorageNodeHealth::kReadOnly),
            MakeCandidate("store-low-capacity",
                          64,
                          storedemo::StorageNodeHealth::kHealthy)};
        request.chunks.push_back(storedemo::UploadChunkInput{
            .chunk_index = 0,
            .offset = 0,
            .payload = payload});
        return request;
    }

    const storedemo::PlacementNodeExclusion *FindExclusion(
        const std::vector<storedemo::PlacementNodeExclusion> &excluded_nodes,
        const std::string &node_id)
    {
        for (const auto &entry : excluded_nodes)
        {
            if (entry.node_id == node_id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    bool ContainsReason(const std::vector<std::string> &reasons,
                        const std::string &expected)
    {
        for (const auto &reason : reasons)
        {
            if (reason.find(expected) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    TEST(IntegratedObjectStorageRecoveryTest,
         NoHealthyOrCapacitySufficientStorageFailsUploadAndKeepsObjectInvisible)
    {
        raftdemo::MetadataStateMachine machine;
        auto metadata_client =
            std::make_shared<storedemo::test::InMemoryUploadMetadataClient>(machine);
        auto chunk_writer =
            std::make_shared<storedemo::test::LocalStoreUploadChunkWriter>();

        const std::string payload = MakeSyntheticUploadPayload();
        const auto request = MakeNoHealthyStorageRequest(payload);

        storedemo::UploadCoordinator coordinator(metadata_client, chunk_writer);
        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_FALSE(result.committed);
        EXPECT_TRUE(result.pending_object_possible);
        EXPECT_FALSE(result.orphan_chunk_possible);
        EXPECT_TRUE(result.committed_chunks.empty());
        EXPECT_TRUE(result.cleanup_candidates.empty());
        EXPECT_EQ(metadata_client->create_calls(), 1U);
        EXPECT_EQ(metadata_client->commit_calls(), 0U);
        EXPECT_EQ(chunk_writer->write_calls(), 0U);
        EXPECT_NE(result.error_detail.find("PlacementManager failed for chunk"),
                  std::string::npos);
        EXPECT_NE(result.error_detail.find(
                      "eligible storage nodes are fewer than requested replica_count"),
                  std::string::npos);

        ASSERT_EQ(result.chunk_executions.size(), 1U);
        const auto &chunk_execution = result.chunk_executions.front();
        EXPECT_TRUE(chunk_execution.placement_decision.replica_nodes.empty());
        EXPECT_TRUE(chunk_execution.replica_results.empty());
        EXPECT_TRUE(ContainsReason(chunk_execution.placement_decision.reasons,
                                   "selection failed because eligible nodes were insufficient"));

        const auto *unavailable_exclusion =
            FindExclusion(chunk_execution.placement_decision.excluded_nodes,
                          "store-unavailable");
        ASSERT_NE(unavailable_exclusion, nullptr);
        EXPECT_EQ(unavailable_exclusion->reason,
                  "node health is not writable: Unavailable");

        const auto *readonly_exclusion =
            FindExclusion(chunk_execution.placement_decision.excluded_nodes,
                          "store-readonly");
        ASSERT_NE(readonly_exclusion, nullptr);
        EXPECT_EQ(readonly_exclusion->reason,
                  "node health is not writable: ReadOnly");

        const auto *capacity_exclusion =
            FindExclusion(chunk_execution.placement_decision.excluded_nodes,
                          "store-low-capacity");
        ASSERT_NE(capacity_exclusion, nullptr);
        EXPECT_EQ(capacity_exclusion->reason,
                  "node capacity is insufficient for requested chunk");

        // 当前 upload flow 失败在 placement 阶段；没有可执行副本集合时不能继续写 chunk，
        // 也不能让对象进入 COMMITTED 可见路径。
        const auto head = machine.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto stored_object =
            machine.FindObject(request.bucket, request.object_key);
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsPending());
        EXPECT_TRUE(stored_object->chunks.empty());

        const auto stored_chunks =
            machine.FindChunkRefs(request.bucket, request.object_key);
        EXPECT_FALSE(stored_chunks.has_value());
    }
} // namespace
