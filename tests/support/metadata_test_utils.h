#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "raft/common/metadata_command.h"
#include "raft/node/raft_node.h"

namespace raftdemo::test
{
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

    inline MetadataCommand MakeDeleteBucketCommand(const std::string &bucket,
                                                   const std::string &request_id,
                                                   const bool if_empty = true)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kDeleteBucket;
        command.request_id = request_id;
        command.delete_bucket = DeleteBucketCommandPayload{bucket, if_empty};
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

    inline MetadataCommand MakeCommitObjectCommand(const std::string &bucket,
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

    inline MetadataCommand MakeAbortObjectCommand(const std::string &bucket,
                                                  const std::string &object_key,
                                                  const std::string &object_id,
                                                  const std::string &request_id,
                                                  const std::uint64_t abort_time = 1710000006)
    {
        MetadataCommand command;
        command.command_type = MetadataCommandType::kAbortObject;
        command.request_id = request_id;
        command.abort_object = AbortObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1};
        command.request_context = RequestRecord{
            request_id,
            MetadataRequestType::kAbortObject,
            bucket,
            object_key,
            "accepted",
            0,
            abort_time,
            std::nullopt};
        return command;
    }

    inline MetadataCommand MakeDeleteObjectCommand(const std::string &bucket,
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

    inline MetadataRecord MakeLegacyCreateRecord(const std::string &object_key,
                                                 const std::string &request_id,
                                                 const std::string &payload = "payload")
    {
        MetadataRecord record;
        record.object_key = object_key;
        record.object_size = 16;
        record.chunk_size = 8;
        record.chunk_count = 2;
        record.checksum = "checksum";
        record.mock_locations = {"node-a", "node-b"};
        record.payload = payload;
        record.create_request_id = request_id;
        return record;
    }

    inline MetadataCommand MakeLegacyCommitCommand(const std::string &object_key,
                                                   const std::string &request_id,
                                                   const std::string &commit_info = "commit-note")
    {
        MetadataCommand command;
        command.operation = MetadataOperation::kCommit;
        command.request_id = request_id;
        command.object_key = object_key;
        command.commit_info = commit_info;
        return command;
    }

    inline std::filesystem::path MakeSnapshotPath(const std::string &filename)
    {
        const std::filesystem::path dir = "tmp/metadata-state-machine-tests";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / filename;
    }

    inline NodeConfig MakeSingleNodeConfig(const std::filesystem::path &root)
    {
        NodeConfig cfg;
        cfg.node_id = 1;
        cfg.address = "127.0.0.1:0";
        cfg.election_timeout_min = std::chrono::milliseconds(200);
        cfg.election_timeout_max = std::chrono::milliseconds(350);
        cfg.heartbeat_interval = std::chrono::milliseconds(60);
        cfg.rpc_deadline = std::chrono::milliseconds(250);
        cfg.data_dir = (root / "data" / "node_1").string();
        return cfg;
    }

    inline snapshotConfig MakeSingleNodeSnapshotConfig(const std::filesystem::path &root)
    {
        snapshotConfig cfg;
        cfg.enabled = false;
        cfg.snapshot_dir = (root / "snapshots" / "node_1").string();
        cfg.load_on_startup = false;
        cfg.file_prefix = "snapshot";
        return cfg;
    }

    template <typename T>
    inline void WritePod(std::ofstream &out, const T &value)
    {
        out.write(reinterpret_cast<const char *>(&value), sizeof(T));
    }
} // namespace raftdemo::test
