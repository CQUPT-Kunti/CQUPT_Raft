#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "store/common/store_types.h"

namespace storedemo
{
    enum class DurableFileErrorCode : std::uint8_t
    {
        kOk = 0,
        kDiskFull = 1,
        kPermissionDenied = 2,
        kIoError = 3,
        kChecksumMismatch = 4,
        kCorrupted = 5,
        kPartialWrite = 6,
        kPathInvalid = 7,
        kAtomicPublishFailed = 8,
        kDirectorySyncFailed = 9,
        kTimeout = 10,
        kCancelled = 11,
        kUnsupported = 12,
    };

    enum class DurableFlushMode : std::uint8_t
    {
        kDataOnly = 0,
        kDataAndMetadata = 1,
    };

    enum class DurablePublishMode : std::uint8_t
    {
        kExclusive = 0,
        kReplaceExisting = 1,
    };

    enum class DurablePathType : std::uint8_t
    {
        kChunkData = 0,
        kStagingData = 1,
        kMetadataSidecar = 2,
        kQuarantineData = 3,
        kWorkingFile = 4,
    };

    const char *ToString(DurableFileErrorCode code);
    StorageNodeStatusCode MapDurableFileErrorCode(DurableFileErrorCode code);
    bool IsRetriableDurableFileError(DurableFileErrorCode code);

    struct DurableOperationContext
    {
        std::uint64_t timeout_ms{0};
        bool best_effort_cancel{false};
    };

    struct DurableFileResult
    {
        DurableFileErrorCode error{DurableFileErrorCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        std::size_t bytes_transferred{0};
        bool durable_boundary_reached{false};
        bool partial_progress{false};

        [[nodiscard]] bool ok() const
        {
            return error == DurableFileErrorCode::kOk;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct NormalizeDurablePathRequest
    {
        std::filesystem::path relative_path;
        DurablePathType path_type{DurablePathType::kChunkData};
    };

    struct NormalizeDurablePathResponse : DurableFileResult
    {
        std::filesystem::path normalized_path;
    };

    struct OpenStagingWriterRequest
    {
        std::filesystem::path relative_path;
        std::optional<std::uint64_t> expected_size;
        DurableOperationContext context;
    };

    struct DurableAppendRequest
    {
        std::span<const std::byte> buffer{};
        DurableOperationContext context;
    };

    struct DurableFlushRequest
    {
        DurableFlushMode mode{DurableFlushMode::kDataOnly};
        DurableOperationContext context;
    };

    struct DurableCloseRequest
    {
        DurableOperationContext context;
    };

    struct PublishDurableFileRequest
    {
        std::filesystem::path staging_path;
        std::filesystem::path final_path;
        DurablePublishMode mode{DurablePublishMode::kExclusive};
        DurableOperationContext context;
    };

    struct SyncDurableDirectoryRequest
    {
        std::filesystem::path directory_path;
        DurableOperationContext context;
    };

    struct ChunkPathLayout
    {
        std::filesystem::path final_relative_path;
        std::filesystem::path staging_relative_path;

        [[nodiscard]] bool IsValid() const;
    };

    StorageNodeStatusCode NormalizeDurableRelativePath(
        const std::filesystem::path &relative_path,
        std::filesystem::path *out_normalized_relative_path,
        std::string *error_detail = nullptr);

    StorageNodeStatusCode ResolveDurablePathUnderRoot(
        const std::filesystem::path &root_path,
        const std::filesystem::path &relative_path,
        std::filesystem::path *out_resolved_path,
        std::string *error_detail = nullptr);

    StorageNodeStatusCode BuildChunkPathLayout(
        std::string_view chunk_id,
        std::string_view staging_token,
        ChunkPathLayout *out_layout,
        std::string *error_detail = nullptr);

    class DurableFileWriter
    {
    public:
        virtual ~DurableFileWriter();

        virtual DurableFileResult Append(const DurableAppendRequest &request) = 0;
        virtual DurableFileResult Flush(const DurableFlushRequest &request) = 0;
        virtual DurableFileResult Close(const DurableCloseRequest &request) = 0;
        [[nodiscard]] virtual const std::filesystem::path &path() const = 0;
    };

    struct OpenStagingWriterResponse : DurableFileResult
    {
        std::filesystem::path normalized_path;
        std::unique_ptr<DurableFileWriter> writer;
    };

    class DurableFile
    {
    public:
        virtual ~DurableFile();

        virtual NormalizeDurablePathResponse NormalizePath(
            const NormalizeDurablePathRequest &request) = 0;
        virtual OpenStagingWriterResponse OpenStagingWriter(
            const OpenStagingWriterRequest &request) = 0;
        virtual DurableFileResult PublishStagedFile(
            const PublishDurableFileRequest &request) = 0;
        virtual DurableFileResult SyncDirectory(
            const SyncDurableDirectoryRequest &request) = 0;
    };

    class LinuxDurableFile : public DurableFile
    {
    public:
        explicit LinuxDurableFile(std::filesystem::path root_path);
        ~LinuxDurableFile() override;

        NormalizeDurablePathResponse NormalizePath(
            const NormalizeDurablePathRequest &request) override;
        OpenStagingWriterResponse OpenStagingWriter(
            const OpenStagingWriterRequest &request) override;
        DurableFileResult PublishStagedFile(
            const PublishDurableFileRequest &request) override;
        DurableFileResult SyncDirectory(
            const SyncDurableDirectoryRequest &request) override;

        [[nodiscard]] const std::filesystem::path &root_path() const;

    private:
        std::filesystem::path root_path_;
    };

    class WindowsDurableFile : public DurableFile
    {
    public:
        explicit WindowsDurableFile(std::filesystem::path root_path);
        ~WindowsDurableFile() override;

        NormalizeDurablePathResponse NormalizePath(
            const NormalizeDurablePathRequest &request) override;
        OpenStagingWriterResponse OpenStagingWriter(
            const OpenStagingWriterRequest &request) override;
        DurableFileResult PublishStagedFile(
            const PublishDurableFileRequest &request) override;
        DurableFileResult SyncDirectory(
            const SyncDurableDirectoryRequest &request) override;

        [[nodiscard]] const std::filesystem::path &root_path() const;

    private:
        std::filesystem::path root_path_;
    };
}
