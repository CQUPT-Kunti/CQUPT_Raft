#include "store/io/durable_file.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace storedemo
{
    namespace
    {
        void SetErrorDetail(std::string *error_detail, const std::string &detail)
        {
            if (error_detail != nullptr)
            {
                *error_detail = detail;
            }
        }

        std::string BuildErrnoDetail(const char *operation,
                                     const std::filesystem::path &path,
                                     const int error_value)
        {
            std::string detail(operation);
            if (!path.empty())
            {
                detail.append(" failed for ");
                detail.append(path.string());
            }
            detail.append(": ");
            detail.append(std::strerror(error_value));
            return detail;
        }

        DurableFileErrorCode MapErrnoToDurableFileErrorCode(const int error_value,
                                                            const bool path_context)
        {
            switch (error_value)
            {
            case 0:
                return DurableFileErrorCode::kOk;
            case ENOSPC:
#ifdef EDQUOT
            case EDQUOT:
#endif
                return DurableFileErrorCode::kDiskFull;
            case EACCES:
            case EPERM:
            case EROFS:
                return DurableFileErrorCode::kPermissionDenied;
            case ETIMEDOUT:
                return DurableFileErrorCode::kTimeout;
#ifdef ECANCELED
            case ECANCELED:
                return DurableFileErrorCode::kCancelled;
#endif
            case EXDEV:
                return DurableFileErrorCode::kAtomicPublishFailed;
            case EIO:
                return DurableFileErrorCode::kIoError;
            case EINTR:
                return DurableFileErrorCode::kIoError;
            case ENOENT:
            case ENOTDIR:
            case ELOOP:
            case ENAMETOOLONG:
            case EINVAL:
                return path_context ? DurableFileErrorCode::kPathInvalid
                                    : DurableFileErrorCode::kIoError;
            default:
                return DurableFileErrorCode::kIoError;
            }
        }

        StorageNodeStatusCode MapDurableFileErrorCodeImpl(const DurableFileErrorCode code)
        {
            switch (code)
            {
            case DurableFileErrorCode::kOk:
                return StorageNodeStatusCode::kOk;
            case DurableFileErrorCode::kDiskFull:
                return StorageNodeStatusCode::kDiskFull;
            case DurableFileErrorCode::kPermissionDenied:
                return StorageNodeStatusCode::kPermissionDenied;
            case DurableFileErrorCode::kIoError:
            case DurableFileErrorCode::kPartialWrite:
            case DurableFileErrorCode::kAtomicPublishFailed:
            case DurableFileErrorCode::kDirectorySyncFailed:
                return StorageNodeStatusCode::kIoError;
            case DurableFileErrorCode::kChecksumMismatch:
                return StorageNodeStatusCode::kChecksumMismatch;
            case DurableFileErrorCode::kCorrupted:
                return StorageNodeStatusCode::kCorrupted;
            case DurableFileErrorCode::kPathInvalid:
                return StorageNodeStatusCode::kInvalidArgument;
            case DurableFileErrorCode::kTimeout:
                return StorageNodeStatusCode::kTimeout;
            case DurableFileErrorCode::kCancelled:
                return StorageNodeStatusCode::kCancelled;
            case DurableFileErrorCode::kUnsupported:
                return StorageNodeStatusCode::kUnsupported;
            }

            return StorageNodeStatusCode::kInvalidArgument;
        }

        bool IsPathWithinRoot(const std::filesystem::path &root_path,
                              const std::filesystem::path &candidate_path)
        {
            auto root_it = root_path.begin();
            auto candidate_it = candidate_path.begin();
            for (; root_it != root_path.end() && candidate_it != candidate_path.end();
                 ++root_it, ++candidate_it)
            {
                if (*root_it != *candidate_it)
                {
                    return false;
                }
            }
            return root_it == root_path.end();
        }

        std::optional<std::filesystem::path> ResolvePathWithinRoot(
            const std::filesystem::path &root_path,
            const std::filesystem::path &input_path)
        {
            if (input_path.empty())
            {
                return std::nullopt;
            }

            if (!input_path.is_absolute() && !input_path.has_root_name())
            {
                return (root_path / input_path.lexically_normal()).lexically_normal();
            }

            const auto normalized_absolute = input_path.lexically_normal();
            if (!IsPathWithinRoot(root_path, normalized_absolute))
            {
                return std::nullopt;
            }
            return normalized_absolute;
        }

        DurableFileResult UnsupportedPlatformResult(const char *operation)
        {
            DurableFileResult result;
            result.error = DurableFileErrorCode::kUnsupported;
            result.error_detail = std::string(operation) + " is only implemented on Linux";
            return result;
        }

        NormalizeDurablePathResponse MakeUnsupportedNormalizeResponse(const char *operation)
        {
            NormalizeDurablePathResponse response;
            response.error = DurableFileErrorCode::kUnsupported;
            response.error_detail = std::string(operation) + " is only implemented on Linux";
            return response;
        }

        OpenStagingWriterResponse MakeUnsupportedOpenResponse(const char *operation)
        {
            OpenStagingWriterResponse response;
            response.error = DurableFileErrorCode::kUnsupported;
            response.error_detail = std::string(operation) + " is only implemented on Linux";
            return response;
        }
#ifdef __linux__
        class LinuxDurableFileWriter : public DurableFileWriter
        {
        public:
            LinuxDurableFileWriter(int file_descriptor, std::filesystem::path path)
                : file_descriptor_(file_descriptor), path_(std::move(path))
            {
            }

            ~LinuxDurableFileWriter() override
            {
                if (file_descriptor_ >= 0)
                {
                    ::close(file_descriptor_);
                }
            }

            DurableFileResult Append(const DurableAppendRequest &request) override
            {
                DurableFileResult result;
                if (file_descriptor_ < 0)
                {
                    result.error = DurableFileErrorCode::kIoError;
                    result.error_detail = "append on closed durable file writer";
                    return result;
                }

                const auto *buffer = reinterpret_cast<const std::byte *>(request.buffer.data());
                std::size_t total_written = 0;
                while (total_written < request.buffer.size())
                {
                    const auto remaining =
                        request.buffer.size() - total_written;
                    const ssize_t write_result = ::write(file_descriptor_,
                                                         buffer + total_written,
                                                         remaining);
                    if (write_result > 0)
                    {
                        total_written += static_cast<std::size_t>(write_result);
                        continue;
                    }

                    if (write_result == 0)
                    {
                        result.error = DurableFileErrorCode::kPartialWrite;
                        result.error_detail = "write returned zero bytes";
                        result.bytes_transferred = total_written;
                        result.partial_progress = total_written > 0;
                        return result;
                    }

                    const int error_value = errno;
                    if (error_value == EINTR)
                    {
                        continue;
                    }

                    result.bytes_transferred = total_written;
                    result.partial_progress = total_written > 0;
                    result.error = total_written > 0
                                       ? DurableFileErrorCode::kPartialWrite
                                       : MapErrnoToDurableFileErrorCode(error_value, false);
                    result.error_detail = BuildErrnoDetail("write", path_, error_value);
                    return result;
                }

                result.bytes_transferred = total_written;
                return result;
            }

            DurableFileResult Flush(const DurableFlushRequest &request) override
            {
                DurableFileResult result;
                if (file_descriptor_ < 0)
                {
                    result.error = DurableFileErrorCode::kIoError;
                    result.error_detail = "flush on closed durable file writer";
                    return result;
                }

                const int sync_result =
                    request.mode == DurableFlushMode::kDataOnly ? ::fdatasync(file_descriptor_)
                                                                : ::fsync(file_descriptor_);
                if (sync_result != 0)
                {
                    const int error_value = errno;
                    result.error = MapErrnoToDurableFileErrorCode(error_value, false);
                    result.error_detail = BuildErrnoDetail("flush", path_, error_value);
                    return result;
                }

                result.durable_boundary_reached = true;
                return result;
            }

            DurableFileResult Close(const DurableCloseRequest &) override
            {
                DurableFileResult result;
                if (file_descriptor_ < 0)
                {
                    return result;
                }

                if (::close(file_descriptor_) != 0)
                {
                    const int error_value = errno;
                    result.error = MapErrnoToDurableFileErrorCode(error_value, false);
                    result.error_detail = BuildErrnoDetail("close", path_, error_value);
                    file_descriptor_ = -1;
                    return result;
                }

                file_descriptor_ = -1;
                return result;
            }

            const std::filesystem::path &path() const override
            {
                return path_;
            }

        private:
            int file_descriptor_{-1};
            std::filesystem::path path_;
        };
#endif
    }

    const char *ToString(const DurableFileErrorCode code)
    {
        switch (code)
        {
        case DurableFileErrorCode::kOk:
            return "Ok";
        case DurableFileErrorCode::kDiskFull:
            return "DiskFull";
        case DurableFileErrorCode::kPermissionDenied:
            return "PermissionDenied";
        case DurableFileErrorCode::kIoError:
            return "IoError";
        case DurableFileErrorCode::kChecksumMismatch:
            return "ChecksumMismatch";
        case DurableFileErrorCode::kCorrupted:
            return "Corrupted";
        case DurableFileErrorCode::kPartialWrite:
            return "PartialWrite";
        case DurableFileErrorCode::kPathInvalid:
            return "PathInvalid";
        case DurableFileErrorCode::kAtomicPublishFailed:
            return "AtomicPublishFailed";
        case DurableFileErrorCode::kDirectorySyncFailed:
            return "DirectorySyncFailed";
        case DurableFileErrorCode::kTimeout:
            return "Timeout";
        case DurableFileErrorCode::kCancelled:
            return "Cancelled";
        case DurableFileErrorCode::kUnsupported:
            return "Unsupported";
        }

        return "Unknown";
    }

    StorageNodeStatusCode MapDurableFileErrorCode(const DurableFileErrorCode code)
    {
        return MapDurableFileErrorCodeImpl(code);
    }

    bool IsRetriableDurableFileError(const DurableFileErrorCode code)
    {
        switch (code)
        {
        case DurableFileErrorCode::kIoError:
        case DurableFileErrorCode::kPartialWrite:
        case DurableFileErrorCode::kAtomicPublishFailed:
        case DurableFileErrorCode::kDirectorySyncFailed:
        case DurableFileErrorCode::kTimeout:
            return true;
        case DurableFileErrorCode::kOk:
        case DurableFileErrorCode::kDiskFull:
        case DurableFileErrorCode::kPermissionDenied:
        case DurableFileErrorCode::kChecksumMismatch:
        case DurableFileErrorCode::kCorrupted:
        case DurableFileErrorCode::kPathInvalid:
        case DurableFileErrorCode::kCancelled:
        case DurableFileErrorCode::kUnsupported:
            return false;
        }

        return false;
    }

    StorageNodeStatusCode DurableFileResult::status_code() const
    {
        return MapDurableFileErrorCodeImpl(error);
    }

    DurableFileWriter::~DurableFileWriter() = default;

    DurableFile::~DurableFile() = default;

    LinuxDurableFile::LinuxDurableFile(std::filesystem::path root_path)
        : root_path_(std::filesystem::absolute(std::move(root_path)).lexically_normal())
    {
    }

    LinuxDurableFile::~LinuxDurableFile() = default;

    NormalizeDurablePathResponse LinuxDurableFile::NormalizePath(
        const NormalizeDurablePathRequest &request)
    {
        NormalizeDurablePathResponse response;
#ifndef __linux__
        response = MakeUnsupportedNormalizeResponse("NormalizePath");
        return response;
#else
        if (request.relative_path.empty())
        {
            response.error = DurableFileErrorCode::kPathInvalid;
            response.error_detail = "relative path must not be empty";
            return response;
        }

        if (request.relative_path.is_absolute() || request.relative_path.has_root_name())
        {
            response.error = DurableFileErrorCode::kPathInvalid;
            response.error_detail = "absolute path is not allowed";
            return response;
        }

        for (const auto &part : request.relative_path)
        {
            if (part == "." || part == "..")
            {
                response.error = DurableFileErrorCode::kPathInvalid;
                response.error_detail = "path traversal is not allowed";
                return response;
            }
        }

        const auto normalized_relative = request.relative_path.lexically_normal();
        if (normalized_relative.empty())
        {
            response.error = DurableFileErrorCode::kPathInvalid;
            response.error_detail = "normalized path must not be empty";
            return response;
        }

        response.normalized_path = (root_path_ / normalized_relative).lexically_normal();
        if (!IsPathWithinRoot(root_path_, response.normalized_path))
        {
            response.error = DurableFileErrorCode::kPathInvalid;
            response.error_detail = "normalized path escapes durable file root";
            response.normalized_path.clear();
            return response;
        }

        return response;
#endif
    }

    OpenStagingWriterResponse LinuxDurableFile::OpenStagingWriter(
        const OpenStagingWriterRequest &request)
    {
        OpenStagingWriterResponse response;
#ifndef __linux__
        response = MakeUnsupportedOpenResponse("OpenStagingWriter");
        return response;
#else
        auto normalized = NormalizePath(
            NormalizeDurablePathRequest{
                .relative_path = request.relative_path,
                .path_type = DurablePathType::kStagingData});
        response.error = normalized.error;
        response.error_detail = normalized.error_detail;
        response.normalized_path = normalized.normalized_path;
        if (!normalized.ok())
        {
            return response;
        }

        const auto parent_path = response.normalized_path.parent_path();
        if (!parent_path.empty())
        {
            std::error_code create_ec;
            std::filesystem::create_directories(parent_path, create_ec);
            if (create_ec)
            {
                response.error = MapErrnoToDurableFileErrorCode(create_ec.value(), true);
                response.error_detail = "create directories failed for " +
                                        parent_path.string() + ": " +
                                        create_ec.message();
                return response;
            }
        }

        const int open_flags = O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC;
        const int file_descriptor =
            ::open(response.normalized_path.c_str(), open_flags, 0644);
        if (file_descriptor < 0)
        {
            const int error_value = errno;
            response.error = MapErrnoToDurableFileErrorCode(error_value, true);
            response.error_detail =
                BuildErrnoDetail("open", response.normalized_path, error_value);
            return response;
        }

        response.writer = std::make_unique<LinuxDurableFileWriter>(
            file_descriptor, response.normalized_path);
        return response;
#endif
    }

    DurableFileResult LinuxDurableFile::PublishStagedFile(
        const PublishDurableFileRequest &request)
    {
        DurableFileResult result;
#ifndef __linux__
        return UnsupportedPlatformResult("PublishStagedFile");
#else
        const auto staging_path = ResolvePathWithinRoot(root_path_, request.staging_path);
        if (!staging_path.has_value())
        {
            result.error = DurableFileErrorCode::kPathInvalid;
            result.error_detail = "staging path escapes durable file root";
            return result;
        }

        const auto final_path = ResolvePathWithinRoot(root_path_, request.final_path);
        if (!final_path.has_value())
        {
            result.error = DurableFileErrorCode::kPathInvalid;
            result.error_detail = "final path escapes durable file root";
            return result;
        }

        const auto final_parent_path = final_path->parent_path();
        if (!final_parent_path.empty())
        {
            std::error_code create_ec;
            std::filesystem::create_directories(final_parent_path, create_ec);
            if (create_ec)
            {
                result.error = MapErrnoToDurableFileErrorCode(create_ec.value(), true);
                result.error_detail = "create directories failed for " +
                                      final_parent_path.string() + ": " +
                                      create_ec.message();
                return result;
            }
        }

        if (request.mode == DurablePublishMode::kExclusive &&
            std::filesystem::exists(*final_path))
        {
            result.error = DurableFileErrorCode::kAtomicPublishFailed;
            result.error_detail = "final path already exists: " + final_path->string();
            return result;
        }

        if (::rename(staging_path->c_str(), final_path->c_str()) != 0)
        {
            const int error_value = errno;
            result.error = MapErrnoToDurableFileErrorCode(error_value, false);
            if (error_value == EXDEV)
            {
                result.error = DurableFileErrorCode::kAtomicPublishFailed;
            }
            result.error_detail = BuildErrnoDetail("rename", *final_path, error_value);
            return result;
        }

        result.durable_boundary_reached = true;
        return result;
#endif
    }

    DurableFileResult LinuxDurableFile::SyncDirectory(
        const SyncDurableDirectoryRequest &request)
    {
        DurableFileResult result;
#ifndef __linux__
        return UnsupportedPlatformResult("SyncDirectory");
#else
        const auto directory_path = request.directory_path.empty()
                                        ? std::optional<std::filesystem::path>(root_path_)
                                        : ResolvePathWithinRoot(root_path_, request.directory_path);
        if (!directory_path.has_value())
        {
            result.error = DurableFileErrorCode::kPathInvalid;
            result.error_detail = "directory path escapes durable file root";
            return result;
        }

        const int directory_fd =
            ::open(directory_path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0)
        {
            const int error_value = errno;
            result.error = MapErrnoToDurableFileErrorCode(error_value, true);
            if (result.error == DurableFileErrorCode::kIoError)
            {
                result.error = DurableFileErrorCode::kDirectorySyncFailed;
            }
            result.error_detail =
                BuildErrnoDetail("open directory", *directory_path, error_value);
            return result;
        }

        if (::fsync(directory_fd) != 0)
        {
            const int error_value = errno;
            result.error = DurableFileErrorCode::kDirectorySyncFailed;
            result.error_detail =
                BuildErrnoDetail("fsync directory", *directory_path, error_value);
            ::close(directory_fd);
            return result;
        }

        if (::close(directory_fd) != 0)
        {
            const int error_value = errno;
            result.error = DurableFileErrorCode::kDirectorySyncFailed;
            result.error_detail =
                BuildErrnoDetail("close directory", *directory_path, error_value);
            return result;
        }

        result.durable_boundary_reached = true;
        return result;
#endif
    }

    const std::filesystem::path &LinuxDurableFile::root_path() const
    {
        return root_path_;
    }
}
