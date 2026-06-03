#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        bool RequiredDurabilityContractSatisfied(const DurableFileResult &result)
        {
            if (!result.ok())
            {
                return result.error != DurableFileErrorCode::kOk;
            }

            return result.durable_boundary_reached;
        }

        class FakeDurableFileWriter : public DurableFileWriter
        {
        public:
            explicit FakeDurableFileWriter(std::filesystem::path path)
                : path_(std::move(path))
            {
            }

            DurableFileResult append_result;
            DurableFileResult flush_result;
            DurableFileResult close_result;

            DurableFileResult Append(const DurableAppendRequest &) override
            {
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
            NormalizeDurablePathResponse normalize_response;
            OpenStagingWriterResponse open_response;
            DurableFileResult publish_response;
            DurableFileResult sync_response;

            NormalizeDurablePathResponse NormalizePath(
                const NormalizeDurablePathRequest &) override
            {
                return normalize_response;
            }

            OpenStagingWriterResponse OpenStagingWriter(
                const OpenStagingWriterRequest &) override
            {
                return std::move(open_response);
            }

            DurableFileResult PublishStagedFile(
                const PublishDurableFileRequest &) override
            {
                return publish_response;
            }

            DurableFileResult SyncDirectory(
                const SyncDurableDirectoryRequest &) override
            {
                return sync_response;
            }
        };

        TEST(StoreDurableFileTest, ErrorMappingCoversContractCategories)
        {
            EXPECT_STREQ(ToString(DurableFileErrorCode::kAtomicPublishFailed),
                         "AtomicPublishFailed");
            EXPECT_STREQ(ToString(DurableFileErrorCode::kDirectorySyncFailed),
                         "DirectorySyncFailed");

            EXPECT_EQ(MapDurableFileErrorCode(DurableFileErrorCode::kDiskFull),
                      StorageNodeStatusCode::kDiskFull);
            EXPECT_EQ(MapDurableFileErrorCode(DurableFileErrorCode::kPathInvalid),
                      StorageNodeStatusCode::kInvalidArgument);
            EXPECT_EQ(MapDurableFileErrorCode(DurableFileErrorCode::kUnsupported),
                      StorageNodeStatusCode::kUnsupported);

            EXPECT_TRUE(IsRetriableDurableFileError(DurableFileErrorCode::kIoError));
            EXPECT_TRUE(IsRetriableDurableFileError(
                DurableFileErrorCode::kAtomicPublishFailed));
            EXPECT_TRUE(IsRetriableDurableFileError(
                DurableFileErrorCode::kDirectorySyncFailed));
            EXPECT_FALSE(IsRetriableDurableFileError(DurableFileErrorCode::kDiskFull));
            EXPECT_FALSE(IsRetriableDurableFileError(
                DurableFileErrorCode::kChecksumMismatch));
            EXPECT_FALSE(IsRetriableDurableFileError(DurableFileErrorCode::kUnsupported));
        }

        TEST(StoreDurableFileTest, NormalizePathAndOpenWriterExposeExplicitErrors)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_contract");
            FakeDurableFile durable_file;

            durable_file.normalize_response.error = DurableFileErrorCode::kPathInvalid;
            durable_file.normalize_response.error_detail = "path traversal rejected";

            auto normalize_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("../escape"),
                    .path_type = DurablePathType::kChunkData});

            EXPECT_FALSE(normalize_response.ok());
            EXPECT_EQ(normalize_response.status_code(),
                      StorageNodeStatusCode::kInvalidArgument);
            EXPECT_EQ(normalize_response.error, DurableFileErrorCode::kPathInvalid);
            EXPECT_TRUE(normalize_response.normalized_path.empty());

            durable_file.open_response.error = DurableFileErrorCode::kUnsupported;
            durable_file.open_response.error_detail = "writer not implemented";
            durable_file.open_response.normalized_path =
                temp_dir.Path("staging/chunk-1.tmp");

            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/chunk-1.tmp"),
                    .expected_size = 128,
                    .context = {}});

            EXPECT_FALSE(open_response.ok());
            EXPECT_EQ(open_response.status_code(), StorageNodeStatusCode::kUnsupported);
            EXPECT_EQ(open_response.error, DurableFileErrorCode::kUnsupported);
            EXPECT_FALSE(open_response.normalized_path.empty());
            EXPECT_EQ(open_response.writer, nullptr);
        }

        TEST(StoreDurableFileTest, RequiredDurabilityOperationsRejectSilentNoopSuccess)
        {
            DurableFileResult noop_success;
            noop_success.error = DurableFileErrorCode::kOk;
            noop_success.durable_boundary_reached = false;
            noop_success.bytes_transferred = 0;
            noop_success.partial_progress = false;

            EXPECT_FALSE(RequiredDurabilityContractSatisfied(noop_success));

            auto writer =
                std::make_unique<FakeDurableFileWriter>(std::filesystem::path("staging.tmp"));
            writer->flush_result = noop_success;

            EXPECT_FALSE(
                RequiredDurabilityContractSatisfied(writer->Flush(DurableFlushRequest{})));

            FakeDurableFile durable_file;
            durable_file.publish_response = noop_success;
            durable_file.sync_response = noop_success;

            EXPECT_FALSE(RequiredDurabilityContractSatisfied(
                durable_file.PublishStagedFile(PublishDurableFileRequest{})));
            EXPECT_FALSE(RequiredDurabilityContractSatisfied(
                durable_file.SyncDirectory(SyncDurableDirectoryRequest{})));
        }

        TEST(StoreDurableFileTest, RequiredDurabilityOperationsAcceptBoundarySuccessOrExplicitFailure)
        {
            DurableFileResult boundary_success;
            boundary_success.error = DurableFileErrorCode::kOk;
            boundary_success.durable_boundary_reached = true;

            EXPECT_TRUE(RequiredDurabilityContractSatisfied(boundary_success));

            DurableFileResult explicit_failure;
            explicit_failure.error = DurableFileErrorCode::kUnsupported;
            explicit_failure.error_detail = "directory sync unsupported";

            EXPECT_TRUE(RequiredDurabilityContractSatisfied(explicit_failure));
            EXPECT_EQ(explicit_failure.status_code(), StorageNodeStatusCode::kUnsupported);
        }

        TEST(StoreDurableFileTest, AppendCanExposePartialWriteWithoutClaimingDurableBoundary)
        {
            auto writer =
                std::make_unique<FakeDurableFileWriter>(std::filesystem::path("staging.tmp"));
            writer->append_result.error = DurableFileErrorCode::kPartialWrite;
            writer->append_result.bytes_transferred = 7;
            writer->append_result.partial_progress = true;
            writer->append_result.error_detail = "short write";

            const std::byte payload[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
            auto result = writer->Append(
                DurableAppendRequest{.buffer = payload, .context = {}});

            EXPECT_FALSE(result.ok());
            EXPECT_EQ(result.error, DurableFileErrorCode::kPartialWrite);
            EXPECT_EQ(result.status_code(), StorageNodeStatusCode::kIoError);
            EXPECT_EQ(result.bytes_transferred, 7U);
            EXPECT_TRUE(result.partial_progress);
            EXPECT_FALSE(result.durable_boundary_reached);
        }

        TEST(StoreDurableFileTest, NormalizeDurableRelativePathRejectsEscapeReservedNamesAndInvalidCharacters)
        {
            std::filesystem::path normalized_path;
            std::string error_detail;

            EXPECT_EQ(NormalizeDurableRelativePath(
                          std::filesystem::path("chunks/live/chunk-1.chunk"),
                          &normalized_path,
                          &error_detail),
                      StorageNodeStatusCode::kOk);
            EXPECT_EQ(normalized_path,
                      std::filesystem::path("chunks/live/chunk-1.chunk"));

            EXPECT_EQ(NormalizeDurableRelativePath(
                          std::filesystem::path("../escape"),
                          &normalized_path,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);

            EXPECT_EQ(NormalizeDurableRelativePath(
                          std::filesystem::path("/absolute/escape"),
                          &normalized_path,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);

            EXPECT_EQ(NormalizeDurableRelativePath(
                          std::filesystem::path("CON/chunk-1.chunk"),
                          &normalized_path,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);

            EXPECT_EQ(NormalizeDurableRelativePath(
                          std::filesystem::path("chunks/bad:name.chunk"),
                          &normalized_path,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);
        }

        TEST(StoreDurableFileTest, ResolveDurablePathUnderRootKeepsResolvedPathInsideRoot)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_resolve_root");
            std::filesystem::path resolved_path;
            std::string error_detail;

            EXPECT_EQ(ResolveDurablePathUnderRoot(
                          temp_dir.root(),
                          std::filesystem::path("chunks/live/chunk-1.chunk"),
                          &resolved_path,
                          &error_detail),
                      StorageNodeStatusCode::kOk);
            EXPECT_EQ(resolved_path,
                      temp_dir.Path("chunks/live/chunk-1.chunk").lexically_normal());

            EXPECT_EQ(ResolveDurablePathUnderRoot(
                          temp_dir.root(),
                          std::filesystem::path("../escape"),
                          &resolved_path,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);
        }

        TEST(StoreDurableFileTest, BuildChunkPathLayoutCreatesDistinctFinalAndStagingPaths)
        {
            ChunkId chunk_id;
            ASSERT_EQ(MakeChunkId("object-alpha", 7, 3, &chunk_id, nullptr),
                      StorageNodeStatusCode::kOk);

            ChunkPathLayout layout;
            std::string error_detail;
            ASSERT_EQ(BuildChunkPathLayout(
                          chunk_id,
                          "request-42",
                          &layout,
                          &error_detail),
                      StorageNodeStatusCode::kOk);
            EXPECT_TRUE(layout.IsValid());
            EXPECT_NE(layout.final_relative_path, layout.staging_relative_path);
            EXPECT_EQ(layout.final_relative_path.begin()->string(), "chunks");
            EXPECT_NE(layout.final_relative_path.generic_string().find("/live/"),
                      std::string::npos);
            EXPECT_NE(layout.staging_relative_path.generic_string().find("/staging/"),
                      std::string::npos);
            EXPECT_NE(layout.final_relative_path.filename().string().find(chunk_id),
                      std::string::npos);
            EXPECT_NE(layout.staging_relative_path.filename().string().find("request-42"),
                      std::string::npos);
        }

        TEST(StoreDurableFileTest, BuildChunkPathLayoutRejectsInvalidChunkIdAndStagingToken)
        {
            ChunkPathLayout layout;
            std::string error_detail;

            EXPECT_EQ(BuildChunkPathLayout(
                          "bad/chunk",
                          "request-42",
                          &layout,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);

            EXPECT_EQ(BuildChunkPathLayout(
                          "object-alpha~7~3",
                          "../escape",
                          &layout,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);

            EXPECT_EQ(BuildChunkPathLayout(
                          "object-alpha~7~3",
                          "CON",
                          &layout,
                          &error_detail),
                      StorageNodeStatusCode::kInvalidArgument);
        }

#ifdef __linux__
        TEST(StoreDurableFileTest, LinuxDurableFileSupportsFlushPublishAndDirectorySync)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_success");
            LinuxDurableFile durable_file(temp_dir.root());

            const std::string payload = test::MakeChunkPayload(48, "linux-durable");
            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/chunk-1.tmp"),
                    .expected_size = static_cast<std::uint64_t>(payload.size()),
                    .context = {}});

            ASSERT_TRUE(open_response.ok());
            ASSERT_NE(open_response.writer, nullptr);
            EXPECT_EQ(open_response.normalized_path,
                      temp_dir.Path("staging/chunk-1.tmp").lexically_normal());

            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            auto append_result = open_response.writer->Append(
                DurableAppendRequest{
                    .buffer = std::span(payload_bytes, payload.size()),
                    .context = {}});
            EXPECT_TRUE(append_result.ok());
            EXPECT_EQ(append_result.bytes_transferred, payload.size());

            auto flush_result = open_response.writer->Flush(
                DurableFlushRequest{
                    .mode = DurableFlushMode::kDataAndMetadata,
                    .context = {}});
            EXPECT_TRUE(flush_result.ok());
            EXPECT_TRUE(flush_result.durable_boundary_reached);

            auto close_result = open_response.writer->Close(DurableCloseRequest{});
            EXPECT_TRUE(close_result.ok());

            const auto final_relative_path = std::filesystem::path("chunks/live/chunk-1.bin");
            auto publish_result = durable_file.PublishStagedFile(
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

            auto sync_result = durable_file.SyncDirectory(
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
        }

        TEST(StoreDurableFileTest, LinuxDurableFileRejectsTraversalAndAbsolutePaths)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_paths");
            LinuxDurableFile durable_file(temp_dir.root());

            auto traversal_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("../escape"),
                    .path_type = DurablePathType::kStagingData});
            EXPECT_FALSE(traversal_response.ok());
            EXPECT_EQ(traversal_response.error, DurableFileErrorCode::kPathInvalid);

            auto absolute_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = temp_dir.Path("absolute/chunk.tmp"),
                    .path_type = DurablePathType::kChunkData});
            EXPECT_FALSE(absolute_response.ok());
            EXPECT_EQ(absolute_response.error, DurableFileErrorCode::kPathInvalid);
        }

        TEST(StoreDurableFileTest,
             LinuxDurableFileAcceptsUtf8PathsAndRejectsReservedOrTooLongSegments)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_utf8");
            LinuxDurableFile durable_file(temp_dir.root());

            const auto utf8_relative_path =
                std::filesystem::path("staging/utf8/路径/块.tmp");
            auto utf8_open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = utf8_relative_path,
                    .expected_size = 8,
                    .context = {}});
            ASSERT_TRUE(utf8_open_response.ok());
            ASSERT_NE(utf8_open_response.writer, nullptr);

            const std::string payload = "utf8-path";
            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            ASSERT_TRUE(utf8_open_response.writer
                            ->Append(DurableAppendRequest{
                                .buffer = std::span(payload_bytes, payload.size()),
                                .context = {}})
                            .ok());
            ASSERT_TRUE(utf8_open_response.writer
                            ->Flush(DurableFlushRequest{
                                .mode = DurableFlushMode::kDataOnly,
                                .context = {}})
                            .ok());
            EXPECT_TRUE(utf8_open_response.writer->Close(DurableCloseRequest{}).ok());
            EXPECT_TRUE(std::filesystem::exists(utf8_open_response.normalized_path));

            auto reserved_name_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("CON/chunk.tmp"),
                    .path_type = DurablePathType::kChunkData});
            EXPECT_FALSE(reserved_name_response.ok());
            EXPECT_EQ(reserved_name_response.error, DurableFileErrorCode::kPathInvalid);

            const std::string long_segment(300, 'a');
            auto long_path_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging") /
                                     std::filesystem::path(long_segment + ".tmp"),
                    .expected_size = 4,
                    .context = {}});
            EXPECT_FALSE(long_path_response.ok());
            EXPECT_EQ(long_path_response.error, DurableFileErrorCode::kPathInvalid);
        }

        TEST(StoreDurableFileTest,
             LinuxDurableFileMapsPermissionDeniedWhenOpeningStagingPath)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_permissions");
            LinuxDurableFile durable_file(temp_dir.root());

            const auto locked_directory = temp_dir.Path("locked");
            std::error_code permissions_error;
            std::filesystem::create_directories(locked_directory, permissions_error);
            ASSERT_FALSE(permissions_error) << permissions_error.message();

            const auto original_permissions =
                std::filesystem::status(locked_directory, permissions_error).permissions();
            ASSERT_FALSE(permissions_error) << permissions_error.message();

            std::filesystem::permissions(
                locked_directory,
                std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_exec,
                std::filesystem::perm_options::replace,
                permissions_error);
            ASSERT_FALSE(permissions_error) << permissions_error.message();

            const auto restore_permissions = [&]()
            {
                std::error_code restore_error;
                std::filesystem::permissions(locked_directory,
                                             original_permissions,
                                             std::filesystem::perm_options::replace,
                                             restore_error);
            };

            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("locked/chunk.tmp"),
                    .expected_size = 16,
                    .context = {}});

            restore_permissions();

            EXPECT_FALSE(open_response.ok());
            EXPECT_EQ(open_response.error, DurableFileErrorCode::kPermissionDenied);
        }

        TEST(StoreDurableFileTest, LinuxDurableFileExclusivePublishRejectsExistingTarget)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_publish_conflict");
            LinuxDurableFile durable_file(temp_dir.root());

            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/chunk-2.tmp"),
                    .expected_size = 8,
                    .context = {}});
            ASSERT_TRUE(open_response.ok());
            ASSERT_NE(open_response.writer, nullptr);

            const std::string payload = "conflict";
            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            ASSERT_TRUE(open_response.writer
                            ->Append(DurableAppendRequest{
                                .buffer = std::span(payload_bytes, payload.size()),
                                .context = {}})
                            .ok());
            ASSERT_TRUE(open_response.writer
                            ->Flush(DurableFlushRequest{
                                .mode = DurableFlushMode::kDataOnly,
                                .context = {}})
                            .ok());
            ASSERT_TRUE(open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto final_path = temp_dir.Path("chunks/live/chunk-2.bin");
            std::filesystem::create_directories(final_path.parent_path());
            {
                std::ofstream existing_file(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(existing_file.is_open());
                existing_file << "existing";
            }

            auto publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = open_response.normalized_path,
                    .final_path = final_path,
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            EXPECT_FALSE(publish_result.ok());
            EXPECT_EQ(publish_result.error, DurableFileErrorCode::kAtomicPublishFailed);
            EXPECT_TRUE(std::filesystem::exists(open_response.normalized_path));
            EXPECT_TRUE(std::filesystem::exists(final_path));
        }

        TEST(StoreDurableFileTest, LinuxDurableFileSyncDirectoryRejectsMissingDirectory)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_linux_missing_dir");
            LinuxDurableFile durable_file(temp_dir.root());

            auto sync_result = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = std::filesystem::path("missing/dir"),
                    .context = {}});
            EXPECT_FALSE(sync_result.ok());
            EXPECT_EQ(sync_result.error, DurableFileErrorCode::kPathInvalid);
        }
#endif

#ifdef _WIN32
        TEST(StoreDurableFileTest, WindowsDurableFileSupportsFlushAndPublish)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_windows_success");
            WindowsDurableFile durable_file(temp_dir.root());

            const std::string payload = test::MakeChunkPayload(64, "windows-durable");
            auto open_response = durable_file.OpenStagingWriter(
                OpenStagingWriterRequest{
                    .relative_path = std::filesystem::path("staging/chunk-1.tmp"),
                    .expected_size = static_cast<std::uint64_t>(payload.size()),
                    .context = {}});

            ASSERT_TRUE(open_response.ok());
            ASSERT_NE(open_response.writer, nullptr);

            const auto *payload_bytes =
                reinterpret_cast<const std::byte *>(payload.data());
            auto append_result = open_response.writer->Append(
                DurableAppendRequest{
                    .buffer = std::span(payload_bytes, payload.size()),
                    .context = {}});
            EXPECT_TRUE(append_result.ok());
            EXPECT_EQ(append_result.bytes_transferred, payload.size());

            auto flush_result = open_response.writer->Flush(
                DurableFlushRequest{
                    .mode = DurableFlushMode::kDataAndMetadata,
                    .context = {}});
            EXPECT_TRUE(flush_result.ok());
            EXPECT_TRUE(flush_result.durable_boundary_reached);

            EXPECT_TRUE(open_response.writer->Close(DurableCloseRequest{}).ok());

            const auto final_relative_path = std::filesystem::path("chunks/live/chunk-1.bin");
            auto publish_result = durable_file.PublishStagedFile(
                PublishDurableFileRequest{
                    .staging_path = open_response.normalized_path,
                    .final_path = final_relative_path,
                    .mode = DurablePublishMode::kExclusive,
                    .context = {}});
            EXPECT_TRUE(publish_result.ok());
            EXPECT_TRUE(publish_result.durable_boundary_reached);

            const auto final_path = temp_dir.Path(final_relative_path.string());
            EXPECT_TRUE(std::filesystem::exists(final_path));

            auto sync_result = durable_file.SyncDirectory(
                SyncDurableDirectoryRequest{
                    .directory_path = final_path.parent_path(),
                    .context = {}});
            EXPECT_FALSE(sync_result.ok());
            EXPECT_EQ(sync_result.error, DurableFileErrorCode::kUnsupported);
        }

        TEST(StoreDurableFileTest, WindowsDurableFileRejectsTraversalReservedNamesAndAbsolutePaths)
        {
            test::ScopedStoreTestDir temp_dir("store_durable_file_windows_paths");
            WindowsDurableFile durable_file(temp_dir.root());

            auto traversal_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("../escape"),
                    .path_type = DurablePathType::kStagingData});
            EXPECT_FALSE(traversal_response.ok());
            EXPECT_EQ(traversal_response.error, DurableFileErrorCode::kPathInvalid);

            auto reserved_name_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = std::filesystem::path("CON/chunk.tmp"),
                    .path_type = DurablePathType::kChunkData});
            EXPECT_FALSE(reserved_name_response.ok());
            EXPECT_EQ(reserved_name_response.error, DurableFileErrorCode::kPathInvalid);

            auto absolute_response = durable_file.NormalizePath(
                NormalizeDurablePathRequest{
                    .relative_path = temp_dir.Path("absolute/chunk.tmp"),
                    .path_type = DurablePathType::kChunkData});
            EXPECT_FALSE(absolute_response.ok());
            EXPECT_EQ(absolute_response.error, DurableFileErrorCode::kPathInvalid);
        }
#endif
    }
}
