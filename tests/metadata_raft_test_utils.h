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
#include "support/metadata_test_utils.h"

namespace raftdemo::test
{
    struct ExpectedRecoveredMetadataObject
    {
        std::string object_key;
        std::string object_id;
        std::size_t expected_chunk_count{0};
        bool deleted{false};
    };

    struct MetadataRecoveryExpectation
    {
        std::string bucket;
        std::vector<ExpectedRecoveredMetadataObject> objects;
        std::vector<std::string> visible_keys;
        std::size_t expected_request_count{0};
        std::size_t expected_tombstone_count{0};
        std::uint64_t expected_last_applied_index{0};
    };

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

    inline ProposeResult ProposeMetadataCommand(
        const std::shared_ptr<RaftNode> &leader,
        const MetadataCommand &command)
    {
        return leader->ProposeMetadata(SerializeMetadataCommand(command));
    }

    inline bool ProposeMetadataCommandWithRetry(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const MetadataCommand &command,
        const std::chrono::milliseconds timeout,
        ProposeResult *final_result = nullptr,
        const std::vector<std::size_t> &excluded = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        ProposeResult last_result;

        while (std::chrono::steady_clock::now() < deadline)
        {
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (IsExcludedNode(i, excluded))
                {
                    continue;
                }

                const auto &node = nodes[i];
                if (node == nullptr)
                {
                    continue;
                }

                if (node->Describe().find("role=Leader") == std::string::npos)
                {
                    continue;
                }

                last_result = ProposeMetadataCommand(node, command);
                if (last_result.Ok())
                {
                    if (final_result != nullptr)
                    {
                        *final_result = last_result;
                    }
                    return true;
                }

                if (last_result.status == ProposeStatus::kInvalidCommand ||
                    last_result.status == ProposeStatus::kApplyFailed ||
                    last_result.status == ProposeStatus::kCommitFailed)
                {
                    if (final_result != nullptr)
                    {
                        *final_result = last_result;
                    }
                    return false;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (final_result != nullptr)
        {
            *final_result = last_result;
        }
        return false;
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

    inline bool WaitUntilAllMetadataRecoveryMatches(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        const MetadataRecoveryExpectation &expectation,
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

                const auto bucket_record = state_machine->FindBucket(expectation.bucket);
                if (!bucket_record.has_value() || !bucket_record->IsActive())
                {
                    ok = false;
                    break;
                }

                if (state_machine->RequestCount() != expectation.expected_request_count ||
                    state_machine->TombstoneCount() != expectation.expected_tombstone_count ||
                    state_machine->LastAppliedIndex() < expectation.expected_last_applied_index)
                {
                    ok = false;
                    break;
                }

                for (const auto &object : expectation.objects)
                {
                    if (!object.deleted)
                    {
                        const auto response = state_machine->HeadObject(
                            {.bucket = expectation.bucket, .object_key = object.object_key});
                        if (!response.result.Ok() || !response.record.has_value() ||
                            !response.record->IsCommitted() ||
                            response.record->object_id != object.object_id)
                        {
                            ok = false;
                            break;
                        }

                        const auto indexed_object_id =
                            state_machine->FindIndexedObjectId(expectation.bucket, object.object_key);
                        if (!indexed_object_id.has_value() ||
                            *indexed_object_id != object.object_id)
                        {
                            ok = false;
                            break;
                        }

                        const auto chunks =
                            state_machine->FindChunkRefs(expectation.bucket, object.object_key);
                        if (!chunks.has_value() ||
                            chunks->size() != object.expected_chunk_count)
                        {
                            ok = false;
                            break;
                        }
                    }
                    else
                    {
                        const auto response = state_machine->HeadObject(
                            {.bucket = expectation.bucket, .object_key = object.object_key});
                        if (response.result.code != MetadataStatusCode::kNotFound ||
                            response.record.has_value())
                        {
                            ok = false;
                            break;
                        }

                        const auto internal_record =
                            state_machine->FindObject(expectation.bucket, object.object_key);
                        if (!internal_record.has_value() ||
                            !internal_record->IsDeleted() ||
                            internal_record->object_id != object.object_id)
                        {
                            ok = false;
                            break;
                        }

                        if (state_machine->FindIndexedObjectId(expectation.bucket,
                                                               object.object_key)
                                .has_value() ||
                            state_machine->FindChunkRefs(expectation.bucket,
                                                         object.object_key)
                                .has_value())
                        {
                            ok = false;
                            break;
                        }
                    }
                }

                if (!ok)
                {
                    break;
                }

                const auto listed = state_machine->ListObjects(
                    {.bucket = expectation.bucket,
                     .prefix = "",
                     .limit = std::nullopt,
                     .continuation_token = ""});
                if (!listed.result.Ok() ||
                    listed.records.size() != expectation.visible_keys.size())
                {
                    ok = false;
                    break;
                }

                for (std::size_t key_index = 0; key_index < expectation.visible_keys.size();
                     ++key_index)
                {
                    if (listed.records[key_index].object_key !=
                            expectation.visible_keys[key_index] ||
                        !listed.records[key_index].IsCommitted())
                    {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                {
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
