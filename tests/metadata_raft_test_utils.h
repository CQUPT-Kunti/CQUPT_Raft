#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

namespace raftdemo::test
{
    inline bool IsExcludedNode(const std::size_t index,
                               const std::vector<std::size_t> &excluded)
    {
        for (const std::size_t excluded_index : excluded)
        {
            if (index == excluded_index)
            {
                return true;
            }
        }
        return false;
    }

    inline MetadataCommand MakeCreateBucketCommand(const std::string &bucket,
                                                   const std::string &request_id,
                                                   const std::uint64_t create_time = 1710000000)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kCreateBucket;
        command.request_id = request_id;
        command.create_bucket = CreateBucketCommandPayload{
            BucketRecord{bucket, create_time, false, std::nullopt}};
        command.request_context = RequestRecord{
            request_id,
            MetadataRequestType::kCreateBucket,
            bucket,
            "",
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    inline MetadataCommand MakeCreateObjectCommand(const std::string &bucket,
                                                   const std::string &object_key,
                                                   const std::string &object_id,
                                                   const std::string &request_id,
                                                   const std::uint64_t create_time = 1710000001)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = CreateObjectCommandPayload{
            ObjectRecord{bucket,
                         object_key,
                         object_id,
                         1,
                         64,
                         "etag-" + object_id,
                         ObjectState::PENDING,
                         {},
                         create_time,
                         std::nullopt,
                         std::nullopt}};
        command.request_context = RequestRecord{
            request_id,
            MetadataRequestType::kCreateObject,
            bucket,
            object_key,
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    inline MetadataCommand MakeCommitObjectCommand(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t commit_time = 1710000005)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            512,
            "etag-commit-" + object_id,
            {ChunkRef{"chunk-a", 0, 256, {"node-a", "node-b"}, "checksum-a"},
             ChunkRef{"chunk-b", 256, 256, {"node-c"}, "checksum-b"}},
            commit_time};
        command.request_context = RequestRecord{
            request_id,
            MetadataRequestType::kCommitObject,
            bucket,
            object_key,
            "accepted",
            0,
            commit_time,
            std::nullopt};
        return command;
    }

    inline MetadataCommand MakeDeleteObjectCommand(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t delete_time = 1710000007)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kDeleteObject;
        command.request_id = request_id;
        command.delete_object = DeleteObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            delete_time};
        command.request_context = RequestRecord{
            request_id,
            MetadataRequestType::kDeleteObject,
            bucket,
            object_key,
            "accepted",
            0,
            delete_time,
            std::nullopt};
        return command;
    }

    inline ProposeResult ProposeMetadataCommand(
        const std::shared_ptr<RaftNode> &leader,
        const MetadataCommand &command)
    {
        return leader->ProposeMetadata(SerializeMetadataCommand(command));
    }

    inline bool WaitUntilAllCommittedObject(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::size_t expected_chunk_count,
        const std::uint64_t expected_last_applied_index,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool ok = true;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcludedNode(i, excluded))
                {
                    continue;
                }

                const auto &node = nodes[i];
                if (!node)
                {
                    continue;
                }

                const MetadataStateMachine *state_machine = node->GetMetadataStateMachineV2();
                if (state_machine == nullptr)
                {
                    ok = false;
                    break;
                }

                const auto bucket_record = state_machine->FindBucket(bucket);
                if (!bucket_record.has_value() || !bucket_record->IsActive())
                {
                    ok = false;
                    break;
                }

                const auto response = state_machine->HeadObject(
                    {.bucket = bucket, .object_key = object_key});
                if (!response.result.Ok() || !response.record.has_value() ||
                    !response.record->IsCommitted() ||
                    response.record->object_id != object_id)
                {
                    ok = false;
                    break;
                }

                const auto indexed_object_id =
                    state_machine->FindIndexedObjectId(bucket, object_key);
                if (!indexed_object_id.has_value() ||
                    *indexed_object_id != object_id)
                {
                    ok = false;
                    break;
                }

                const auto chunks = state_machine->FindChunkRefs(bucket, object_key);
                if (!chunks.has_value() || chunks->size() != expected_chunk_count)
                {
                    ok = false;
                    break;
                }

                if (state_machine->LastAppliedIndex() < expected_last_applied_index)
                {
                    ok = false;
                    break;
                }
            }

            if (ok)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

    inline bool WaitUntilAllDeletedObjectHidden(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::uint64_t expected_last_applied_index,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool ok = true;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcludedNode(i, excluded))
                {
                    continue;
                }

                const auto &node = nodes[i];
                if (!node)
                {
                    continue;
                }

                const MetadataStateMachine *state_machine = node->GetMetadataStateMachineV2();
                if (state_machine == nullptr)
                {
                    ok = false;
                    break;
                }

                const auto response = state_machine->HeadObject(
                    {.bucket = bucket, .object_key = object_key});
                if (response.result.code != MetadataStatusCode::kNotFound ||
                    response.record.has_value())
                {
                    ok = false;
                    break;
                }

                const auto internal_record = state_machine->FindObject(bucket, object_key);
                if (!internal_record.has_value() || !internal_record->IsDeleted() ||
                    internal_record->object_id != object_id)
                {
                    ok = false;
                    break;
                }

                if (state_machine->FindIndexedObjectId(bucket, object_key).has_value() ||
                    state_machine->FindChunkRefs(bucket, object_key).has_value())
                {
                    ok = false;
                    break;
                }

                const auto list = state_machine->ListObjects(
                    {.bucket = bucket, .prefix = "", .limit = std::nullopt, .continuation_token = ""});
                if (!list.result.Ok())
                {
                    ok = false;
                    break;
                }
                for (const auto &record : list.records)
                {
                    if (record.object_key == object_key)
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                {
                    break;
                }

                if (state_machine->LastAppliedIndex() < expected_last_applied_index)
                {
                    ok = false;
                    break;
                }
            }

            if (ok)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

    inline bool WaitUntilAllListObjectsMatch(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const std::string &bucket,
        const std::string &prefix,
        const std::vector<std::string> &expected_keys,
        const std::uint64_t expected_last_applied_index,
        const std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool ok = true;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcludedNode(i, excluded))
                {
                    continue;
                }

                const auto &node = nodes[i];
                if (!node)
                {
                    continue;
                }

                const MetadataStateMachine *state_machine = node->GetMetadataStateMachineV2();
                if (state_machine == nullptr)
                {
                    ok = false;
                    break;
                }

                const auto response = state_machine->ListObjects(
                    {.bucket = bucket, .prefix = prefix, .limit = std::nullopt, .continuation_token = ""});
                if (!response.result.Ok() || response.records.size() != expected_keys.size())
                {
                    ok = false;
                    break;
                }

                for (std::size_t i = 0; i < expected_keys.size(); ++i)
                {
                    if (response.records[i].object_key != expected_keys[i] ||
                        !response.records[i].IsCommitted())
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                {
                    break;
                }

                if (state_machine->LastAppliedIndex() < expected_last_applied_index)
                {
                    ok = false;
                    break;
                }
            }

            if (ok)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

} // namespace raftdemo::test
