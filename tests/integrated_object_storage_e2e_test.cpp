#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "support/metadata_test_utils.h"

#include "metadata.pb.h"
#include "store/common/store_types.h"
#include "storage_node.pb.h"

#include <google/protobuf/descriptor.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using raftdemo::test::ApplyMetadataCommand;
    using raftdemo::test::MakeCreateBucketCommand;
    using raftdemo::test::MakeSnapshotPath;

    struct HappyPathE2EScaffoldWorkspace
    {
        std::filesystem::path root;
        std::filesystem::path source_path;
        std::filesystem::path download_path;

        ~HappyPathE2EScaffoldWorkspace()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    bool DescriptorHasBytesField(const google::protobuf::Descriptor &descriptor)
    {
        for (int index = 0; index < descriptor.field_count(); ++index)
        {
            if (descriptor.field(index)->type() ==
                google::protobuf::FieldDescriptor::TYPE_BYTES)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<char> ReadBinaryFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open snapshot file: " + path.string());
        }

        return std::vector<char>(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
    }

    std::string MakeHappyPathFixturePayload()
    {
        std::string payload;
        payload.reserve(64 * 1024);
        for (std::size_t index = 0; index < 64 * 1024; ++index)
        {
            payload.push_back(static_cast<char>((index * 31 + 17) % 251));
        }
        return payload;
    }

    void WriteBinaryFileOrThrow(const std::filesystem::path &path,
                                const std::string &content)
    {
        std::error_code create_ec;
        std::filesystem::create_directories(path.parent_path(), create_ec);
        if (create_ec)
        {
            throw std::runtime_error("failed to create directories for test file: " +
                                     path.string() + ": " + create_ec.message());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open output file: " + path.string());
        }

        output.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output.good())
        {
            throw std::runtime_error("failed to write output file: " + path.string());
        }
    }

    std::string ReadBinaryFileToStringOrThrow(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary test file: " +
                                     path.string());
        }

        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    std::string ComputeFileSha256OrThrow(const std::filesystem::path &path)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const std::string payload = ReadBinaryFileToStringOrThrow(path);
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute scaffold SHA-256 for " +
                                     path.string() + ": " + error_detail);
        }

        return checksum.value;
    }

    std::string ComputePayloadSha256OrThrow(const std::string &payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute scaffold SHA-256: " +
                                     error_detail);
        }

        return checksum.value;
    }

    std::string MakeCorruptedPayloadCopy(const std::string &payload,
                                         const std::size_t offset)
    {
        if (payload.empty())
        {
            throw std::runtime_error("cannot corrupt empty payload");
        }

        std::string corrupted = payload;
        const std::size_t index = offset % corrupted.size();
        corrupted[index] = static_cast<char>(corrupted[index] ^ 0x5A);
        if (corrupted[index] == payload[index])
        {
            corrupted[index] = static_cast<char>(corrupted[index] ^ 0x01);
        }
        return corrupted;
    }

    HappyPathE2EScaffoldWorkspace MakeHappyPathE2EScaffoldWorkspace()
    {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        HappyPathE2EScaffoldWorkspace workspace;
        workspace.root = std::filesystem::temp_directory_path() /
                         "cqupt_integrated_object_storage_e2e" /
                         ("t026-" + std::to_string(now_ns));
        workspace.source_path = workspace.root / "input" / "fixture.bin";
        workspace.download_path = workspace.root / "output" / "fixture.download.bin";
        return workspace;
    }

    std::vector<raftdemo::ChunkRef> MakeAuditChunks()
    {
        return {
            raftdemo::ChunkRef{
                "chunk-t022-0", 0, 4096, {"store-a", "store-b"}, "sha256:chunk-t022-0"},
            raftdemo::ChunkRef{
                "chunk-t022-1", 4096, 2048, {"store-b"}, "sha256:chunk-t022-1"}};
    }

    raftdemo::MetadataCommand MakeCreateObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = "create-object-t022";
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{"bucket-t022",
                                   "objects/boundary-audit.bin",
                                   "obj-t022",
                                   3,
                                   6144,
                                   "sha256:object-t022",
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   1717555200001ULL,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            "create-object-t022",
            raftdemo::MetadataRequestType::kCreateObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t022";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t022",
            "objects/boundary-audit.bin",
            "obj-t022",
            3,
            6144,
            "sha256:object-t022",
            MakeAuditChunks(),
            1717555200999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t022",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            1717555200999ULL};
        return command;
    }

    std::vector<raftdemo::ChunkRef> MakeChecksumMismatchChunks(
        const std::string &payload)
    {
        const std::size_t first_chunk_size = payload.size() / 2;
        const std::size_t second_chunk_size = payload.size() - first_chunk_size;
        return {
            raftdemo::ChunkRef{"chunk-t028-0",
                               0,
                               static_cast<std::uint64_t>(first_chunk_size),
                               {"store-a", "store-b"},
                               ComputePayloadSha256OrThrow(
                                   payload.substr(0, first_chunk_size))},
            raftdemo::ChunkRef{"chunk-t028-1",
                               static_cast<std::uint64_t>(first_chunk_size),
                               static_cast<std::uint64_t>(second_chunk_size),
                               {"store-b"},
                               ComputePayloadSha256OrThrow(
                                   payload.substr(first_chunk_size))}};
    }

    raftdemo::MetadataCommand MakeChecksumMismatchCommitCommand(
        const std::vector<raftdemo::ChunkRef> &chunks,
        const std::uint64_t object_size,
        const std::string &object_checksum)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t028";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t028",
            "objects/checksum-mismatch.bin",
            "obj-t028",
            1,
            object_size,
            object_checksum,
            chunks,
            1717555300999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t028",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t028",
            "objects/checksum-mismatch.bin",
            "accepted",
            0,
            1717555300001ULL,
            1717555300999ULL};
        return command;
    }

    void ExpectChunkRefsEqual(const std::vector<raftdemo::ChunkRef> &actual,
                              const std::vector<raftdemo::ChunkRef> &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(actual[index].chunk_id, expected[index].chunk_id);
            EXPECT_EQ(actual[index].offset, expected[index].offset);
            EXPECT_EQ(actual[index].size, expected[index].size);
            EXPECT_EQ(actual[index].replica_nodes, expected[index].replica_nodes);
            EXPECT_EQ(actual[index].checksum, expected[index].checksum);
        }
    }
} // namespace

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataControlPlaneDescriptorsExcludeRawPayloadBytes)
{
    ASSERT_NE(raft::CreateObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::CommitObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::ChunkRef::descriptor(), nullptr);
    ASSERT_NE(raft::ObjectRecord::descriptor(), nullptr);
    ASSERT_NE(raft::HeadObjectResponse::descriptor(), nullptr);
    ASSERT_NE(raft::ListObjectsResponse::descriptor(), nullptr);

    EXPECT_EQ(raft::CreateObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::CommitObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ChunkRef::descriptor()->FindFieldByName("payload"), nullptr);
    EXPECT_EQ(raft::ObjectRecord::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::HeadObjectResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ListObjectsResponse::descriptor()->FindFieldByName("payload"),
              nullptr);

    EXPECT_FALSE(DescriptorHasBytesField(*raft::CreateObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::CommitObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ChunkRef::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ObjectRecord::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::HeadObjectResponse::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ListObjectsResponse::descriptor()));

    ASSERT_NE(storage::WriteChunkRequest::descriptor(), nullptr);
    ASSERT_NE(storage::ReadChunkResponse::descriptor(), nullptr);
    EXPECT_NE(storage::WriteChunkRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_NE(storage::ReadChunkResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_TRUE(DescriptorHasBytesField(*storage::WriteChunkRequest::descriptor()));
    EXPECT_TRUE(DescriptorHasBytesField(*storage::ReadChunkResponse::descriptor()));
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataCommandsSerializeOnlyManifestFacts)
{
    const raftdemo::MetadataCommand create_command = MakeCreateObjectAuditCommand();
    const raftdemo::MetadataCommand commit_command = MakeCommitObjectAuditCommand();

    std::string error;
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(create_command, &error)) << error;
    error.clear();
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(commit_command, &error)) << error;

    const std::string create_encoded =
        raftdemo::SerializeMetadataCommand(create_command);
    const std::string commit_encoded =
        raftdemo::SerializeMetadataCommand(commit_command);

    EXPECT_EQ(create_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(commit_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(create_encoded.find("payload"), std::string::npos);
    EXPECT_EQ(commit_encoded.find("payload"), std::string::npos);

    EXPECT_NE(create_encoded.find("target_bucket=bucket-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_object_id=obj-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_size=6144"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_count=2"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_0_id=chunk-t022-0"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_offset=4096"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_checksum=sha256:chunk-t022-1"),
              std::string::npos);

    raftdemo::MetadataCommand parsed_commit;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(commit_encoded, &parsed_commit));
    ASSERT_TRUE(parsed_commit.commit_object.has_value());
    EXPECT_EQ(parsed_commit.commit_object->bucket, "bucket-t022");
    EXPECT_EQ(parsed_commit.commit_object->object_key,
              "objects/boundary-audit.bin");
    EXPECT_EQ(parsed_commit.commit_object->object_id, "obj-t022");
    EXPECT_EQ(parsed_commit.commit_object->size, 6144U);
    ExpectChunkRefsEqual(parsed_commit.commit_object->chunks, MakeAuditChunks());
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataSnapshotRoundTripKeepsManifestFactsOnly)
{
    raftdemo::MetadataStateMachine machine;
    const std::vector<raftdemo::ChunkRef> expected_chunks = MakeAuditChunks();

    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t022",
                                                             "create-bucket-t022"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateObjectAuditCommand())
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCommitObjectAuditCommand())
                    .Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("t022-payload-boundary.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);

    const auto save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk) << save.message;

    const std::vector<char> snapshot_bytes = ReadBinaryFile(snapshot_path);
    const std::string snapshot_text(snapshot_bytes.begin(), snapshot_bytes.end());
    EXPECT_EQ(snapshot_text.find("record_payload="), std::string::npos);
    EXPECT_EQ(snapshot_text.find("payload"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-0"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-1"), std::string::npos);
    EXPECT_NE(snapshot_text.find("sha256:chunk-t022-1"), std::string::npos);

    raftdemo::MetadataStateMachine restored;
    const auto load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk) << load.message;

    const auto head = restored.HeadObject(
        {.bucket = "bucket-t022", .object_key = "objects/boundary-audit.bin"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_TRUE(head.record->IsCommitted());
    EXPECT_EQ(head.record->object_id, "obj-t022");
    EXPECT_EQ(head.record->version, 3U);
    EXPECT_EQ(head.record->size, 6144U);
    EXPECT_EQ(head.record->etag, "sha256:object-t022");
    ExpectChunkRefsEqual(head.record->chunks, expected_chunks);

    const auto chunk_refs = restored.FindChunkRefs("bucket-t022",
                                                   "objects/boundary-audit.bin");
    ASSERT_TRUE(chunk_refs.has_value());
    ExpectChunkRefsEqual(*chunk_refs, expected_chunks);
}

TEST(IntegratedObjectStorageE2ETest,
     HappyPathUploadDownloadScaffoldPreparesRealFileAndChecksumExpectation)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string payload = MakeHappyPathFixturePayload();

    WriteBinaryFileOrThrow(workspace.source_path, payload);

    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    EXPECT_FALSE(std::filesystem::exists(workspace.download_path));

    const std::string source_checksum =
        ComputeFileSha256OrThrow(workspace.source_path);
    EXPECT_EQ(source_checksum.size(), storedemo::kSha256DigestHexChars);
    EXPECT_FALSE(source_checksum.empty());

    const std::string payload_round_trip =
        ReadBinaryFileToStringOrThrow(workspace.source_path);
    EXPECT_EQ(payload_round_trip, payload);

    const auto object_key =
        std::string("objects/") + workspace.source_path.filename().string();
    EXPECT_EQ(object_key, "objects/fixture.bin");
    EXPECT_EQ(std::filesystem::file_size(workspace.source_path),
              static_cast<std::uintmax_t>(payload.size()));

    // T026 只建立 happy-path E2E scaffold：真实输入文件、目标下载路径和
    // SHA-256 比对入口已经就位；真实 upload/download/manifest 流程由后续任务接入。
}

TEST(IntegratedObjectStorageE2ETest,
     DISABLED_HappyPathUploadDownloadRoundTripViaIntegratedObjectStorage)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    WriteBinaryFileOrThrow(workspace.source_path, MakeHappyPathFixturePayload());
    const std::string expected_sha256 =
        ComputeFileSha256OrThrow(workspace.source_path);

    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    ASSERT_FALSE(expected_sha256.empty());
    ASSERT_FALSE(std::filesystem::exists(workspace.download_path));

    GTEST_SKIP()
        << "T026 仅提供 happy-path E2E scaffold。启用该 round-trip 用例需要后续任务完成："
        << "T029/T030 object_transfer、T031/T032 metadata transfer adapter、"
        << "T033/T034 storage transfer adapter、T035 ViewNode discovery 接入、"
        << "T036 manifest-driven download reconstruction、T037 storage_client upload/download。";
}

TEST(IntegratedObjectStorageE2ETest,
     ManifestVisibilityPendingHiddenCommittedVisible)
{
    raftdemo::MetadataStateMachine machine;
    std::uint64_t index = 1;

    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t027",
                                                             "create-bucket-t027"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCreateObjectCommand(
                                         "bucket-t027",
                                         "objects/visibility.bin",
                                         "obj-t027",
                                         "create-object-t027"))
                    .Ok);

    // T027 锁定 manifest 可见性边界：普通可见路径只能来自 MetadataNode
    // 已提交的 COMMITTED manifest，不能从 ViewNode 观测或 StorageNode 本地状态推断。
    const auto pending_head = machine.HeadObject(
        {.bucket = "bucket-t027", .object_key = "objects/visibility.bin"});
    EXPECT_EQ(pending_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(pending_head.record.has_value());

    const auto pending_list =
        machine.ListObjects({.bucket = "bucket-t027", .prefix = "objects/"});
    ASSERT_EQ(pending_list.result.code, raftdemo::MetadataStatusCode::kOk);
    EXPECT_TRUE(pending_list.records.empty());

    const auto pending_chunks =
        machine.FindChunkRefs("bucket-t027", "objects/visibility.bin");
    EXPECT_FALSE(pending_chunks.has_value());

    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCommitObjectCommand(
                                         "bucket-t027",
                                         "objects/visibility.bin",
                                         "obj-t027",
                                         "commit-object-t027"))
                    .Ok);

    const auto committed_head = machine.HeadObject(
        {.bucket = "bucket-t027", .object_key = "objects/visibility.bin"});
    ASSERT_EQ(committed_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head.record.has_value());
    EXPECT_TRUE(committed_head.record->IsCommitted());
    EXPECT_EQ(committed_head.record->object_id, "obj-t027");

    const auto committed_list =
        machine.ListObjects({.bucket = "bucket-t027", .prefix = "objects/"});
    ASSERT_EQ(committed_list.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(committed_list.records.size(), 1U);
    EXPECT_EQ(committed_list.records.front().object_key, "objects/visibility.bin");
    EXPECT_TRUE(committed_list.records.front().IsCommitted());

    const auto committed_chunks =
        machine.FindChunkRefs("bucket-t027", "objects/visibility.bin");
    ASSERT_TRUE(committed_chunks.has_value());
    ExpectChunkRefsEqual(*committed_chunks,
                         raftdemo::test::MakeCommitObjectCommand("bucket-t027",
                                                                 "objects/visibility.bin",
                                                                 "obj-t027",
                                                                 "commit-object-t027")
                             .commit_object->chunks);
}

TEST(IntegratedObjectStorageE2ETest,
     ChecksumMismatchDownloadFailureScaffoldPreparesCommittedManifestAndCorruptChunkFixture)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string source_payload = MakeHappyPathFixturePayload();
    const std::string corrupted_payload =
        MakeCorruptedPayloadCopy(source_payload, source_payload.size() * 3 / 4);
    const std::vector<raftdemo::ChunkRef> expected_chunks =
        MakeChecksumMismatchChunks(source_payload);
    const std::string expected_object_checksum =
        ComputePayloadSha256OrThrow(source_payload);

    raftdemo::MetadataStateMachine machine;
    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t028",
                                                             "create-bucket-t028"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     raftdemo::test::MakeCreateObjectCommand(
                                         "bucket-t028",
                                         "objects/checksum-mismatch.bin",
                                         "obj-t028",
                                         "create-object-t028"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeChecksumMismatchCommitCommand(
                                         expected_chunks,
                                         static_cast<std::uint64_t>(
                                             source_payload.size()),
                                         expected_object_checksum))
                    .Ok);

    const auto committed_head = machine.HeadObject(
        {.bucket = "bucket-t028", .object_key = "objects/checksum-mismatch.bin"});
    ASSERT_EQ(committed_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head.record.has_value());
    EXPECT_TRUE(committed_head.record->IsCommitted());
    EXPECT_EQ(committed_head.record->etag, expected_object_checksum);

    const auto committed_chunks =
        machine.FindChunkRefs("bucket-t028", "objects/checksum-mismatch.bin");
    ASSERT_TRUE(committed_chunks.has_value());
    ExpectChunkRefsEqual(*committed_chunks, expected_chunks);

    WriteBinaryFileOrThrow(workspace.source_path, source_payload);
    const auto corrupted_chunk_path = workspace.root / "chunks" / "chunk-1.bin";
    WriteBinaryFileOrThrow(corrupted_chunk_path,
                           corrupted_payload.substr(source_payload.size() / 2));

    const std::string healthy_first_chunk_checksum = ComputePayloadSha256OrThrow(
        source_payload.substr(0, source_payload.size() / 2));
    const std::string corrupted_second_chunk_checksum =
        ComputeFileSha256OrThrow(corrupted_chunk_path);
    const std::string corrupted_object_checksum =
        ComputePayloadSha256OrThrow(corrupted_payload);

    // T028 先锁定 checksum mismatch 失败前置条件：manifest 提供 committed
    // checksum，损坏 chunk 的实际校验值与 manifest 不一致，后续真实下载实现必须失败。
    EXPECT_EQ(healthy_first_chunk_checksum, expected_chunks[0].checksum);
    EXPECT_NE(corrupted_second_chunk_checksum, expected_chunks[1].checksum);
    EXPECT_NE(corrupted_object_checksum, expected_object_checksum);
    EXPECT_FALSE(std::filesystem::exists(workspace.download_path));
}

TEST(IntegratedObjectStorageE2ETest,
     DISABLED_ChecksumMismatchDownloadFailsWithoutPublishingCorruptedFile)
{
    const auto workspace = MakeHappyPathE2EScaffoldWorkspace();
    const std::string source_payload = MakeHappyPathFixturePayload();
    const std::string corrupted_payload =
        MakeCorruptedPayloadCopy(source_payload, source_payload.size() * 3 / 4);

    WriteBinaryFileOrThrow(workspace.source_path, source_payload);
    ASSERT_TRUE(std::filesystem::exists(workspace.source_path));
    ASSERT_FALSE(std::filesystem::exists(workspace.download_path));
    ASSERT_NE(ComputePayloadSha256OrThrow(corrupted_payload),
              ComputePayloadSha256OrThrow(source_payload));

    GTEST_SKIP()
        << "T028 仅提供 checksum mismatch 下载失败测试骨架。启用该用例需要后续任务完成："
        << "T030 object_transfer 上传/下载会话、T032 metadata transfer adapter、"
        << "T034 storage transfer adapter、T035 ViewNode discovery 接入、"
        << "T036 manifest-driven download reconstruction 与 checksum fail-fast、"
        << "T037 storage_client upload/download。";
}
