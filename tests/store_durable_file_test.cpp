#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
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
    }
}
