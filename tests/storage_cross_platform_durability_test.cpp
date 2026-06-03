#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        enum class MatrixCoverage : std::uint8_t
        {
            kVerifiedHere = 0,
            kDeferredHere = 1,
            kContractOnly = 2,
        };

        struct DurabilityMatrixRow
        {
            std::string name;
            MatrixCoverage coverage{MatrixCoverage::kContractOnly};
        };

        bool RequiredDurabilityContractSatisfied(const DurableFileResult &result)
        {
            if (!result.ok())
            {
                return result.error != DurableFileErrorCode::kOk;
            }

            return result.durable_boundary_reached;
        }

        ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
        {
            ChunkChecksum checksum;
            std::string error_detail;
            const auto status =
                ComputeChunkChecksum(payload, &checksum, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to compute checksum: " + error_detail);
            }
            return checksum;
        }

        ChunkIdentity MakeIdentityOrThrow(const std::string_view object_id,
                                          const std::uint64_t version,
                                          const std::uint32_t chunk_index,
                                          const std::uint64_t offset = 0)
        {
            ChunkId chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(object_id,
                                            version,
                                            chunk_index,
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build chunk id: " + error_detail);
            }

            ChunkIdentity identity;
            identity.chunk_id = std::move(chunk_id);
            identity.object_id = std::string(object_id);
            identity.version = version;
            identity.chunk_index = chunk_index;
            identity.offset = offset;
            return identity;
        }

        WriteChunkRequest MakeWriteRequest(const ChunkIdentity &identity,
                                           const std::string &payload,
                                           const std::string &request_id)
        {
            return WriteChunkRequest{
                .request_id = request_id,
                .identity = identity,
                .expected_size = static_cast<std::uint64_t>(payload.size()),
                .expected_checksum = ComputeChecksumOrThrow(payload),
                .payload = payload};
        }

        class FakeDurableFileWriter : public DurableFileWriter
        {
        public:
            explicit FakeDurableFileWriter(std::filesystem::path path)
                : path_(std::move(path))
            {
            }

            DurableFileResult append_result{
                .error = DurableFileErrorCode::kOk,
                .bytes_transferred = 0,
                .durable_boundary_reached = false};
            DurableFileResult flush_result{
                .error = DurableFileErrorCode::kOk,
                .durable_boundary_reached = true};
            DurableFileResult close_result{
                .error = DurableFileErrorCode::kOk};

            DurableFileResult Append(const DurableAppendRequest &request) override
            {
                append_result.bytes_transferred = request.buffer.size();
                return append_result;
            }

            DurableFileResult Flush(const DurableFlushRequest &) override
            {
                return flush_result;
            }

            DurableFileResult Close(const DurableCloseRequest &) override
            {
                return close_result;
            }

            const std::filesystem::path &path() const override
            {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        class FakeDurableFile : public DurableFile
        {
        public:
            explicit FakeDurableFile(std::filesystem::path root_path)
                : root_path_(std::move(root_path))
            {
            }

            DurableFileResult publish_result{
                .error = DurableFileErrorCode::kOk,
                .durable_boundary_reached = true};
            DurableFileResult sync_result{
                .error = DurableFileErrorCode::kOk,
                .durable_boundary_reached = true};

            NormalizeDurablePathResponse NormalizePath(
                const NormalizeDurablePathRequest &request) override
            {
                NormalizeDurablePathResponse response;
                response.normalized_path =
                    (root_path_ / request.relative_path).lexically_normal();
                return response;
            }

            OpenStagingWriterResponse OpenStagingWriter(
                const OpenStagingWriterRequest &request) override
            {
                OpenStagingWriterResponse response;
                response.normalized_path =
                    (root_path_ / request.relative_path).lexically_normal();
                response.writer =
                    std::make_unique<FakeDurableFileWriter>(response.normalized_path);
                return response;
            }

            DurableFileResult PublishStagedFile(
                const PublishDurableFileRequest &) override
            {
                return publish_result;
            }

            DurableFileResult SyncDirectory(
                const SyncDurableDirectoryRequest &) override
            {
                return sync_result;
            }

        private:
            std::filesystem::path root_path_;
        };

        class LocalDiskChunkStoreDurabilityMatrixTest
            : public ::testing::TestWithParam<const char *>
        {
        };

        std::vector<DurabilityMatrixRow> BuildCurrentPlatformMatrixRows()
        {
            std::vector<DurabilityMatrixRow> rows;
            rows.push_back({"required-op-noop-success-is-not-allowed",
                            MatrixCoverage::kContractOnly});

#if defined(__linux__)
            rows.push_back({"linux-fdatasync-data-only-flush",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-fsync-data-and-metadata-flush",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-parent-directory-sync",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-same-filesystem-publish",
                            MatrixCoverage::kVerifiedHere});
#else
            rows.push_back({"linux-fdatasync-data-only-flush",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-fsync-data-and-metadata-flush",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-parent-directory-sync",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-same-filesystem-publish",
                            MatrixCoverage::kDeferredHere});
#endif

#if defined(_WIN32)
            rows.push_back({"windows-FlushFileBuffers-flush",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-MoveFileEx-exclusive-publish",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-replace-existing-publish-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-long-path-and-utf8-path-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-sharing-violation-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-directory-durability-explicit-unsupported",
                            MatrixCoverage::kVerifiedHere});
#else
            rows.push_back({"windows-FlushFileBuffers-flush",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-MoveFileEx-exclusive-publish",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-replace-existing-publish-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-long-path-and-utf8-path-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-sharing-violation-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-directory-durability-explicit-unsupported",
                            MatrixCoverage::kDeferredHere});
#endif

            return rows;
        }

        TEST(StorageCrossPlatformDurabilityTest,
             MatrixClassifiesLinuxWindowsAndPlatformNeutralCoverage)
        {
            const auto rows = BuildCurrentPlatformMatrixRows();
            ASSERT_GE(rows.size(), 11U);

            for (const auto &row : rows)
            {
                if (row.name.rfind("linux-", 0) == 0)
                {
#if defined(__linux__)
                    EXPECT_EQ(row.coverage, MatrixCoverage::kVerifiedHere)
                        << row.name;
#else
                    EXPECT_EQ(row.coverage, MatrixCoverage::kDeferredHere)
                        << row.name;
#endif
                    continue;
                }

                if (row.name.rfind("windows-", 0) == 0)
                {
#if defined(_WIN32)
                    EXPECT_EQ(row.coverage, MatrixCoverage::kVerifiedHere)
                        << row.name;
#else
                    EXPECT_EQ(row.coverage, MatrixCoverage::kDeferredHere)
                        << row.name;
#endif
                    continue;
                }

                EXPECT_EQ(row.coverage, MatrixCoverage::kContractOnly)
                    << row.name;
            }
        }

        TEST(StorageCrossPlatformDurabilityTest,
             RequiredDurabilityOperationsRejectSilentNoopSuccess)
        {
            const std::vector<std::pair<std::string, DurableFileResult>> required_results = {
                {"flush-noop-success",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kOk,
                     .durable_boundary_reached = false}},
                {"publish-noop-success",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kOk,
                     .durable_boundary_reached = false}},
                {"directory-sync-noop-success",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kOk,
                     .durable_boundary_reached = false}},
                {"directory-sync-explicit-unsupported",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kUnsupported,
                     .durable_boundary_reached = false}},
            };

            for (const auto &[name, result] : required_results)
            {
                if (result.ok())
                {
                    EXPECT_FALSE(RequiredDurabilityContractSatisfied(result)) << name;
                }
                else
                {
                    EXPECT_TRUE(RequiredDurabilityContractSatisfied(result)) << name;
                    EXPECT_NE(result.error, DurableFileErrorCode::kOk) << name;
                }
            }
        }

        TEST(StorageCrossPlatformDurabilityTest,
             LinuxRuntimeVerifiesFlushPublishDirectorySyncAndExplicitErrors)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "Linux runtime durability matrix is only verified on Linux";
#else
            test::ScopedStoreTestDir temp_dir(
                "storage_cross_platform_durability_linux_runtime");
            LinuxDurableFile durable_file(temp_dir.root());

            const std::string payload = test::MakeChunkPayload(48, "linux-matrix");
            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/matrix-1.tmp"),
                    .expected_size = static_cast<std::uint64_t>(payload.size()),
                    .context = {}});
            ASSERT_TRUE(open_response.ok());
            ASSERT_NE(open_response.writer, nullptr);

            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            const auto append_result = open_response.writer->Append(
                DurableAppendRequest{
                    .buffer = std::span(payload_bytes, payload.size()),
                    .context = {}});
            ASSERT_TRUE(append_result.ok());
            EXPECT_EQ(append_result.bytes_transferred, payload.size());

            const auto data_only_flush = open_response.writer->Flush(
                DurableFlushRequest{
                    .mode = DurableFlushMode::kDataOnly,
                    .context = {}});
            EXPECT_TRUE(data_only_flush.ok());
            EXPECT_TRUE(data_only_flush.durable_boundary_reached);

            const auto data_and_metadata_flush = open_response.writer->Flush(
                DurableFlushRequest{
                    .mode = DurableFlushMode::kDataAndMetadata,
                    .context = {}});
            EXPECT_TRUE(data_and_metadata_flush.ok());
            EXPECT_TRUE(data_and_metadata_flush.durable_boundary_reached);

            EXPECT_TRUE(open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto final_relative_path =
                std::filesystem::path("chunks/live/matrix-1.chunk");
            const auto publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = open_response.normalized_path,
                    .final_path = final_relative_path,
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            EXPECT_TRUE(publish_result.ok());
            EXPECT_TRUE(publish_result.durable_boundary_reached);

            const auto final_path = temp_dir.Path(final_relative_path.string());
            EXPECT_FALSE(std::filesystem::exists(open_response.normalized_path));
            ASSERT_TRUE(std::filesystem::exists(final_path));

            const auto sync_result = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = final_path.parent_path(),
                    .context = {}});
            EXPECT_TRUE(sync_result.ok());
            EXPECT_TRUE(sync_result.durable_boundary_reached);

            std::ifstream input(final_path, std::ios::binary);
            ASSERT_TRUE(input.is_open());
            const std::string actual_payload{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            EXPECT_EQ(actual_payload, payload);

            const auto missing_publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = std::filesystem::path("staging/missing.tmp"),
                    .final_path = std::filesystem::path("chunks/live/missing.chunk"),
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            EXPECT_FALSE(missing_publish_result.ok());
            EXPECT_EQ(missing_publish_result.error, DurableFileErrorCode::kIoError);

            const auto invalid_path_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("../escape"),
                    .path_type = DurablePathType::kStagingData});
            EXPECT_FALSE(invalid_path_response.ok());
            EXPECT_EQ(invalid_path_response.error, DurableFileErrorCode::kPathInvalid);

            const auto missing_directory_sync = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = std::filesystem::path("missing/dir"),
                    .context = {}});
            EXPECT_FALSE(missing_directory_sync.ok());
            EXPECT_EQ(missing_directory_sync.error, DurableFileErrorCode::kPathInvalid);
#endif
        }

        TEST(StorageCrossPlatformDurabilityTest,
             WindowsRuntimeMatrixIsDeferredOutsideWindowsAndExplicitAboutPendingValidation)
        {
#if !defined(_WIN32)
            GTEST_SKIP()
                << "Windows durability runtime validation is deferred on this Linux environment: "
                << "FlushFileBuffers, MoveFileEx publish, ReplaceExisting publish contract, "
                << "long path / UTF-8 path, sharing violation, directory durability";
#else
            test::ScopedStoreTestDir temp_dir(
                "storage_cross_platform_durability_windows_runtime");
            WindowsDurableFile durable_file(temp_dir.root());

            const std::string payload = test::MakeChunkPayload(32, "windows-matrix");
            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/matrix-1.tmp"),
                    .expected_size = static_cast<std::uint64_t>(payload.size()),
                    .context = {}});
            ASSERT_TRUE(open_response.ok());
            ASSERT_NE(open_response.writer, nullptr);

            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            ASSERT_TRUE(open_response.writer
                            ->Append(DurableAppendRequest{
                                .buffer = std::span(payload_bytes, payload.size()),
                                .context = {}})
                            .ok());

            const auto flush_result = open_response.writer->Flush(
                DurableFlushRequest{
                    .mode = DurableFlushMode::kDataAndMetadata,
                    .context = {}});
            EXPECT_TRUE(flush_result.ok());
            EXPECT_TRUE(flush_result.durable_boundary_reached);
            EXPECT_TRUE(open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto first_final_path =
                std::filesystem::path("chunks/live/windows-exclusive.chunk");
            const auto first_publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = open_response.normalized_path,
                    .final_path = first_final_path,
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            EXPECT_TRUE(first_publish_result.ok());
            EXPECT_TRUE(first_publish_result.durable_boundary_reached);

            auto replace_open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/matrix-2.tmp"),
                    .expected_size = static_cast<std::uint64_t>(payload.size()),
                    .context = {}});
            ASSERT_TRUE(replace_open_response.ok());
            ASSERT_NE(replace_open_response.writer, nullptr);
            ASSERT_TRUE(replace_open_response.writer
                            ->Append(DurableAppendRequest{
                                .buffer = std::span(payload_bytes, payload.size()),
                                .context = {}})
                            .ok());
            ASSERT_TRUE(replace_open_response.writer
                            ->Flush(DurableFlushRequest{
                                .mode = DurableFlushMode::kDataAndMetadata,
                                .context = {}})
                            .ok());
            ASSERT_TRUE(replace_open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto replace_publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = replace_open_response.normalized_path,
                    .final_path = first_final_path,
                    .mode = DurablePublishMode::kReplaceExisting,
                    .context = {}});
            EXPECT_TRUE(replace_publish_result.ok());
            EXPECT_TRUE(replace_publish_result.durable_boundary_reached);

            const auto directory_sync_result = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = temp_dir.Path("chunks/live"),
                    .context = {}});
            EXPECT_FALSE(directory_sync_result.ok());
            EXPECT_EQ(directory_sync_result.error, DurableFileErrorCode::kUnsupported);
#endif
        }

        TEST_P(LocalDiskChunkStoreDurabilityMatrixTest,
               RequiredStoreDurabilityBoundaryDoesNotAllowSilentNoopSuccess)
        {
            test::ScopedStoreTestDir temp_dir(
                "storage_cross_platform_store_boundary_matrix");
            const auto chunk_index = std::make_shared<ShardedChunkIndex>();
            auto durable_file = std::make_shared<FakeDurableFile>(temp_dir.root());

            if (std::string_view(GetParam()) == "publish")
            {
                durable_file->publish_result.error = DurableFileErrorCode::kOk;
                durable_file->publish_result.durable_boundary_reached = false;
            }
            else
            {
                durable_file->sync_result.error = DurableFileErrorCode::kOk;
                durable_file->sync_result.durable_boundary_reached = false;
            }

            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = "durability-matrix-store",
                .durable_file = durable_file,
                .chunk_index = chunk_index,
                .executor = nullptr});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity =
                MakeIdentityOrThrow("durability-matrix-object", 1, 0, 0);
            const auto response = store.WriteChunk(
                MakeWriteRequest(identity,
                                 test::MakeChunkPayload(24, "durability-store"),
                                 std::string("durability-") +
                                     std::string(GetParam())));

            EXPECT_EQ(response.status, StorageNodeStatusCode::kIoError);
            EXPECT_FALSE(response.ok());

            const auto index_find = chunk_index->Find(identity.chunk_id);
            EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound);
        }

        INSTANTIATE_TEST_SUITE_P(
            PublishAndDirectorySyncBoundary,
            LocalDiskChunkStoreDurabilityMatrixTest,
            ::testing::Values("publish", "sync"));
    } // namespace
} // namespace storedemo
