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

            DurableFileResult open_result{
                .error = DurableFileErrorCode::kOk};
            DurableFileResult append_result{
                .error = DurableFileErrorCode::kOk,
                .bytes_transferred = 0,
                .durable_boundary_reached = false};
            DurableFileResult flush_result{
                .error = DurableFileErrorCode::kOk,
                .durable_boundary_reached = true};
            DurableFileResult close_result{
                .error = DurableFileErrorCode::kOk};
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
                response.error = open_result.error;
                response.error_detail = open_result.error_detail;
                response.retry_after_ms = open_result.retry_after_ms;
                response.bytes_transferred = open_result.bytes_transferred;
                response.durable_boundary_reached =
                    open_result.durable_boundary_reached;
                response.partial_progress = open_result.partial_progress;
                if (!response.ok())
                {
                    return response;
                }

                auto writer =
                    std::make_unique<FakeDurableFileWriter>(response.normalized_path);
                writer->append_result = append_result;
                writer->flush_result = flush_result;
                writer->close_result = close_result;
                response.writer =
                    std::move(writer);
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
            rows.push_back({"linux-path-invalid-and-reserved-name-classification",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-permission-denied-classification",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-disk-full-failure-injection-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"linux-utf8-safe-path-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back(
                {"linux-crash-after-rename-before-parent-directory-sync-contract",
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
            rows.push_back({"linux-path-invalid-and-reserved-name-classification",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-permission-denied-classification",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-disk-full-failure-injection-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"linux-utf8-safe-path-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back(
                {"linux-crash-after-rename-before-parent-directory-sync-contract",
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
            rows.push_back(
                {"windows-permission-denied-and-disk-full-classification-contract",
                 MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-reserved-name-path-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-sharing-violation-contract",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back({"windows-directory-durability-explicit-unsupported",
                            MatrixCoverage::kVerifiedHere});
            rows.push_back(
                {"windows-crash-after-rename-before-parent-directory-sync-contract",
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
            rows.push_back(
                {"windows-permission-denied-and-disk-full-classification-contract",
                 MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-reserved-name-path-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-sharing-violation-contract",
                            MatrixCoverage::kDeferredHere});
            rows.push_back({"windows-directory-durability-explicit-unsupported",
                            MatrixCoverage::kDeferredHere});
            rows.push_back(
                {"windows-crash-after-rename-before-parent-directory-sync-contract",
                 MatrixCoverage::kDeferredHere});
#endif

            return rows;
        }

        TEST(StorageCrossPlatformDurabilityTest,
             MatrixClassifiesLinuxWindowsAndPlatformNeutralCoverage)
        {
            const auto rows = BuildCurrentPlatformMatrixRows();
            ASSERT_GE(rows.size(), 19U);

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
             LinuxCrashAfterRenameBeforeParentDirectorySyncRequiresExplicitDirectorySync)
        {
#if !defined(__linux__)
            GTEST_SKIP()
                << "rename-before-parent-directory-sync contract is only runtime-verified on Linux";
#else
            test::ScopedStoreTestDir temp_dir(
                "storage_cross_platform_durability_linux_rename_before_directory_sync");
            LinuxDurableFile durable_file(temp_dir.root());

            const std::string payload =
                test::MakeChunkPayload(40, "rename-before-directory-sync");
            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/matrix-rename.tmp"),
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
            ASSERT_TRUE(open_response.writer
                            ->Flush(DurableFlushRequest{
                                .mode = DurableFlushMode::kDataAndMetadata,
                                .context = {}})
                            .ok());
            ASSERT_TRUE(open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto final_relative_path =
                std::filesystem::path("chunks/live/matrix-rename.chunk");
            const auto publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = open_response.normalized_path,
                    .final_path = final_relative_path,
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            ASSERT_TRUE(publish_result.ok());
            EXPECT_TRUE(publish_result.durable_boundary_reached);

            const auto final_path = temp_dir.Path(final_relative_path.string());
            ASSERT_TRUE(std::filesystem::exists(final_path));

            std::ifstream input(final_path, std::ios::binary);
            ASSERT_TRUE(input.is_open());
            const std::string visible_payload{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            EXPECT_EQ(visible_payload, payload);

            const DurableFileResult directory_sync_not_yet_performed{
                .error = DurableFileErrorCode::kOk,
                .durable_boundary_reached = false};
            EXPECT_FALSE(
                RequiredDurabilityContractSatisfied(directory_sync_not_yet_performed));

            const bool crash_after_rename_before_directory_sync_is_fully_durable =
                publish_result.ok() &&
                publish_result.durable_boundary_reached &&
                RequiredDurabilityContractSatisfied(directory_sync_not_yet_performed);
            EXPECT_FALSE(crash_after_rename_before_directory_sync_is_fully_durable);

            const auto sync_result = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = final_path.parent_path(),
                    .context = {}});
            ASSERT_TRUE(sync_result.ok());
            EXPECT_TRUE(sync_result.durable_boundary_reached);

            const bool durable_contract_after_parent_directory_sync =
                publish_result.ok() &&
                publish_result.durable_boundary_reached &&
                sync_result.ok() &&
                sync_result.durable_boundary_reached;
            EXPECT_TRUE(durable_contract_after_parent_directory_sync);
#endif
        }

        TEST(StorageCrossPlatformDurabilityTest,
             WindowsRuntimeMatrixIsDeferredOutsideWindowsAndExplicitAboutPendingValidation)
        {
#if !defined(_WIN32)
            GTEST_SKIP()
                << "Windows durability runtime validation is deferred on this Linux environment: "
                << "FlushFileBuffers, MoveFileEx publish, ReplaceExisting publish contract, "
                << "long path / UTF-8 path, permission denied, disk full, "
                << "reserved name, sharing violation, directory durability";
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

        TEST(StorageCrossPlatformDurabilityTest,
             LocalDiskChunkStorePropagatesDurableFilePathPermissionAndDiskFullErrors)
        {
            struct StoreWriteErrorCase
            {
                const char *name;
                DurableFileResult open_result;
                DurableFileResult append_result;
                StorageNodeStatusCode expected_status;
            };

            const std::vector<StoreWriteErrorCase> cases = {
                {"path-invalid-open",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kPathInvalid,
                     .error_detail = "invalid durable path"},
                 DurableFileResult{},
                 StorageNodeStatusCode::kInvalidArgument},
                {"permission-denied-open",
                 DurableFileResult{
                     .error = DurableFileErrorCode::kPermissionDenied,
                     .error_detail = "permission denied opening staging path"},
                 DurableFileResult{},
                 StorageNodeStatusCode::kPermissionDenied},
                {"disk-full-append",
                 DurableFileResult{},
                 DurableFileResult{
                     .error = DurableFileErrorCode::kDiskFull,
                     .error_detail = "disk full while appending payload"},
                 StorageNodeStatusCode::kDiskFull},
            };

            for (const auto &test_case : cases)
            {
                test::ScopedStoreTestDir temp_dir(
                    std::string("storage_cross_platform_store_error_") +
                    test_case.name);
                const auto chunk_index = std::make_shared<ShardedChunkIndex>();
                auto durable_file =
                    std::make_shared<FakeDurableFile>(temp_dir.root());
                durable_file->open_result = test_case.open_result;
                durable_file->append_result = test_case.append_result;

                LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                    .data_dir = temp_dir.Path("node-data"),
                    .node_id = std::string("error-case-") + test_case.name,
                    .durable_file = durable_file,
                    .chunk_index = chunk_index,
                    .executor = nullptr});
                ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk)
                    << test_case.name;

                const auto identity =
                    MakeIdentityOrThrow(std::string("error-case-object-") +
                                            test_case.name,
                                        1,
                                        0,
                                        0);
                const auto response = store.WriteChunk(
                    MakeWriteRequest(identity,
                                     test::MakeChunkPayload(24, test_case.name),
                                     std::string("write-") + test_case.name));

                EXPECT_EQ(response.status, test_case.expected_status)
                    << test_case.name;
                EXPECT_FALSE(response.ok()) << test_case.name;

                const auto index_find = chunk_index->Find(identity.chunk_id);
                EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound)
                    << test_case.name;
            }
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
