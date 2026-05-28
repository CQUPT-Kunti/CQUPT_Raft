#include "store/io/durable_file.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstring>
#include <system_error>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

        std::string PathToUtf8String(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            const auto encoded = path.u8string();
            return std::string(reinterpret_cast<const char *>(encoded.data()),
                               encoded.size());
#else
            return path.string();
#endif
        }

        std::string BuildErrnoDetail(const char *operation,
                                     const std::filesystem::path &path,
                                     const int error_value)
        {
            std::string detail(operation);
            if (!path.empty())
            {
                detail.append(" failed for ");
                detail.append(PathToUtf8String(path));
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
                auto root_part = PathToUtf8String(*root_it);
                auto candidate_part = PathToUtf8String(*candidate_it);
#ifdef _WIN32
                std::transform(root_part.begin(), root_part.end(), root_part.begin(),
                               [](unsigned char value)
                               { return static_cast<char>(std::tolower(value)); });
                std::transform(candidate_part.begin(),
                               candidate_part.end(),
                               candidate_part.begin(),
                               [](unsigned char value)
                               { return static_cast<char>(std::tolower(value)); });
#endif
                if (root_part != candidate_part)
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

        DurableFileResult UnsupportedPlatformResult(const char *operation,
                                                    const char *platform_name)
        {
            DurableFileResult result;
            result.error = DurableFileErrorCode::kUnsupported;
            result.error_detail =
                std::string(operation) + " is only implemented on " + platform_name;
            return result;
        }

        NormalizeDurablePathResponse MakeUnsupportedNormalizeResponse(const char *operation,
                                                                     const char *platform_name)
        {
            NormalizeDurablePathResponse response;
            response.error = DurableFileErrorCode::kUnsupported;
            response.error_detail =
                std::string(operation) + " is only implemented on " + platform_name;
            return response;
        }

        OpenStagingWriterResponse MakeUnsupportedOpenResponse(const char *operation,
                                                             const char *platform_name)
        {
            OpenStagingWriterResponse response;
            response.error = DurableFileErrorCode::kUnsupported;
            response.error_detail =
                std::string(operation) + " is only implemented on " + platform_name;
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

#ifdef _WIN32
        std::string BuildWindowsErrorDetail(const char *operation,
                                            const std::filesystem::path &path,
                                            const DWORD error_value)
        {
            std::string detail(operation);
            if (!path.empty())
            {
                detail.append(" failed for ");
                detail.append(PathToUtf8String(path));
            }
            detail.append(": Win32 error ");
            detail.append(std::to_string(error_value));
            return detail;
        }

        DurableFileErrorCode MapWindowsErrorToDurableFileErrorCode(const DWORD error_value,
                                                                   const bool path_context)
        {
            switch (error_value)
            {
            case ERROR_SUCCESS:
                return DurableFileErrorCode::kOk;
            case ERROR_DISK_FULL:
            case ERROR_HANDLE_DISK_FULL:
                return DurableFileErrorCode::kDiskFull;
            case ERROR_ACCESS_DENIED:
            case ERROR_PRIVILEGE_NOT_HELD:
            case ERROR_WRITE_PROTECT:
                return DurableFileErrorCode::kPermissionDenied;
            case WAIT_TIMEOUT:
            case ERROR_TIMEOUT:
                return DurableFileErrorCode::kTimeout;
            case ERROR_OPERATION_ABORTED:
                return DurableFileErrorCode::kCancelled;
            case ERROR_NOT_SAME_DEVICE:
                return DurableFileErrorCode::kAtomicPublishFailed;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                return DurableFileErrorCode::kPermissionDenied;
            case ERROR_PATH_NOT_FOUND:
            case ERROR_FILE_NOT_FOUND:
            case ERROR_INVALID_NAME:
            case ERROR_BAD_PATHNAME:
            case ERROR_DIRECTORY:
            case ERROR_FILENAME_EXCED_RANGE:
            case ERROR_BUFFER_OVERFLOW:
            case ERROR_INVALID_PARAMETER:
                return path_context ? DurableFileErrorCode::kPathInvalid
                                    : DurableFileErrorCode::kIoError;
            default:
                return DurableFileErrorCode::kIoError;
            }
        }

        std::string TrimWindowsTrailingDotsAndSpaces(std::string value)
        {
            while (!value.empty() &&
                   (value.back() == ' ' || value.back() == '.'))
            {
                value.pop_back();
            }
            return value;
        }

        std::string UppercaseAscii(std::string value)
        {
            std::transform(value.begin(),
                           value.end(),
                           value.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::toupper(ch)); });
            return value;
        }

        bool IsWindowsReservedBaseName(const std::string &base_name)
        {
            static const char *const kReservedNames[] = {
                "CON", "PRN", "AUX", "NUL",
                "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

            for (const char *reserved : kReservedNames)
            {
                if (base_name == reserved)
                {
                    return true;
                }
            }
            return false;
        }

        bool ContainsWindowsInvalidPathChars(const std::string &part)
        {
            return part.find_first_of("<>:\"|?*") != std::string::npos;
        }

        bool IsWindowsReservedPathPart(const std::string &part)
        {
            const auto trimmed = TrimWindowsTrailingDotsAndSpaces(part);
            const auto extension_separator = trimmed.find('.');
            const auto base_name =
                UppercaseAscii(trimmed.substr(0, extension_separator));
            return !base_name.empty() && IsWindowsReservedBaseName(base_name);
        }

        bool IsValidWindowsRelativePathPart(const std::filesystem::path &part)
        {
            const auto part_string = PathToUtf8String(part);
            if (part_string.empty())
            {
                return false;
            }
            if (part_string == "." || part_string == "..")
            {
                return false;
            }
            if (part_string.back() == ' ' || part_string.back() == '.')
            {
                return false;
            }
            if (ContainsWindowsInvalidPathChars(part_string))
            {
                return false;
            }
            if (IsWindowsReservedPathPart(part_string))
            {
                return false;
            }
            return true;
        }

        std::optional<std::wstring> Utf8ToWide(const std::string &utf8)
        {
            if (utf8.empty())
            {
                return std::wstring{};
            }

            if (utf8.size() > static_cast<std::size_t>(INT_MAX))
            {
                return std::nullopt;
            }

            const int required_length = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                utf8.data(),
                static_cast<int>(utf8.size()),
                nullptr,
                0);
            if (required_length <= 0)
            {
                return std::nullopt;
            }

            std::wstring wide(required_length, L'\0');
            const int converted_length = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                utf8.data(),
                static_cast<int>(utf8.size()),
                wide.data(),
                required_length);
            if (converted_length != required_length)
            {
                return std::nullopt;
            }
            return wide;
        }

        std::optional<std::wstring> PrepareWindowsApiPath(
            const std::filesystem::path &path)
        {
            auto preferred = path.lexically_normal();
            preferred.make_preferred();
            const auto utf8 = PathToUtf8String(preferred);
            auto wide_path = Utf8ToWide(utf8);
            if (!wide_path.has_value())
            {
                return std::nullopt;
            }

            if (wide_path->rfind(L"\\\\?\\", 0) == 0)
            {
                return wide_path;
            }

            if (wide_path->rfind(L"\\\\", 0) == 0)
            {
                return std::wstring(L"\\\\?\\UNC\\") + wide_path->substr(2);
            }

            if (wide_path->size() >= 248 && path.is_absolute())
            {
                return std::wstring(L"\\\\?\\") + *wide_path;
            }

            return wide_path;
        }

        class WindowsDurableFileWriter : public DurableFileWriter
        {
        public:
            WindowsDurableFileWriter(HANDLE file_handle, std::filesystem::path path)
                : file_handle_(file_handle), path_(std::move(path))
            {
            }

            ~WindowsDurableFileWriter() override
            {
                if (file_handle_ != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(file_handle_);
                }
            }

            DurableFileResult Append(const DurableAppendRequest &request) override
            {
                DurableFileResult result;
                if (file_handle_ == INVALID_HANDLE_VALUE)
                {
                    result.error = DurableFileErrorCode::kIoError;
                    result.error_detail = "append on closed durable file writer";
                    return result;
                }

                const auto *buffer =
                    reinterpret_cast<const std::byte *>(request.buffer.data());
                std::size_t total_written = 0;
                while (total_written < request.buffer.size())
                {
                    const auto remaining = request.buffer.size() - total_written;
                    const DWORD write_length = static_cast<DWORD>(
                        std::min<std::size_t>(remaining, static_cast<std::size_t>(0xffffffffU)));
                    DWORD bytes_written = 0;
                    if (::WriteFile(file_handle_,
                                    buffer + total_written,
                                    write_length,
                                    &bytes_written,
                                    nullptr) == 0)
                    {
                        const DWORD error_value = ::GetLastError();
                        result.bytes_transferred = total_written;
                        result.partial_progress = total_written > 0;
                        result.error = total_written > 0
                                           ? DurableFileErrorCode::kPartialWrite
                                           : MapWindowsErrorToDurableFileErrorCode(error_value,
                                                                                  false);
                        result.error_detail =
                            BuildWindowsErrorDetail("WriteFile", path_, error_value);
                        return result;
                    }

                    if (bytes_written == 0)
                    {
                        result.error = DurableFileErrorCode::kPartialWrite;
                        result.error_detail = "WriteFile returned zero bytes";
                        result.bytes_transferred = total_written;
                        result.partial_progress = total_written > 0;
                        return result;
                    }

                    total_written += bytes_written;
                }

                result.bytes_transferred = total_written;
                return result;
            }

            DurableFileResult Flush(const DurableFlushRequest &) override
            {
                DurableFileResult result;
                if (file_handle_ == INVALID_HANDLE_VALUE)
                {
                    result.error = DurableFileErrorCode::kIoError;
                    result.error_detail = "flush on closed durable file writer";
                    return result;
                }

                if (::FlushFileBuffers(file_handle_) == 0)
                {
                    const DWORD error_value = ::GetLastError();
                    result.error = MapWindowsErrorToDurableFileErrorCode(error_value, false);
                    result.error_detail =
                        BuildWindowsErrorDetail("FlushFileBuffers", path_, error_value);
                    return result;
                }

                result.durable_boundary_reached = true;
                return result;
            }

            DurableFileResult Close(const DurableCloseRequest &) override
            {
                DurableFileResult result;
                if (file_handle_ == INVALID_HANDLE_VALUE)
                {
                    return result;
                }

                if (::CloseHandle(file_handle_) == 0)
                {
                    const DWORD error_value = ::GetLastError();
                    result.error = MapWindowsErrorToDurableFileErrorCode(error_value, false);
                    result.error_detail =
                        BuildWindowsErrorDetail("CloseHandle", path_, error_value);
                    file_handle_ = INVALID_HANDLE_VALUE;
                    return result;
                }

                file_handle_ = INVALID_HANDLE_VALUE;
                return result;
            }

            const std::filesystem::path &path() const override
            {
                return path_;
            }

        private:
            HANDLE file_handle_{INVALID_HANDLE_VALUE};
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
        response = MakeUnsupportedNormalizeResponse("NormalizePath", "Linux");
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
        response = MakeUnsupportedOpenResponse("OpenStagingWriter", "Linux");
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
        return UnsupportedPlatformResult("PublishStagedFile", "Linux");
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
        return UnsupportedPlatformResult("SyncDirectory", "Linux");
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

    WindowsDurableFile::WindowsDurableFile(std::filesystem::path root_path)
        : root_path_(std::filesystem::absolute(std::move(root_path)).lexically_normal())
    {
    }

    WindowsDurableFile::~WindowsDurableFile() = default;

    NormalizeDurablePathResponse WindowsDurableFile::NormalizePath(
        const NormalizeDurablePathRequest &request)
    {
        NormalizeDurablePathResponse response;
#ifndef _WIN32
        response = MakeUnsupportedNormalizeResponse("NormalizePath", "Windows");
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
            if (!IsValidWindowsRelativePathPart(part))
            {
                response.error = DurableFileErrorCode::kPathInvalid;
                response.error_detail =
                    "Windows path contains traversal, reserved name or invalid character";
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

    OpenStagingWriterResponse WindowsDurableFile::OpenStagingWriter(
        const OpenStagingWriterRequest &request)
    {
        OpenStagingWriterResponse response;
#ifndef _WIN32
        response = MakeUnsupportedOpenResponse("OpenStagingWriter", "Windows");
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
                response.error = MapWindowsErrorToDurableFileErrorCode(
                    static_cast<DWORD>(create_ec.value()), true);
                response.error_detail = "create directories failed for " +
                                        PathToUtf8String(parent_path) + ": " +
                                        create_ec.message();
                return response;
            }
        }

        const auto wide_path = PrepareWindowsApiPath(response.normalized_path);
        if (!wide_path.has_value())
        {
            response.error = DurableFileErrorCode::kPathInvalid;
            response.error_detail = "unable to convert UTF-8 durable path to UTF-16";
            response.normalized_path.clear();
            return response;
        }

        const HANDLE file_handle = ::CreateFileW(
            wide_path->c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file_handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error_value = ::GetLastError();
            response.error = MapWindowsErrorToDurableFileErrorCode(error_value, true);
            response.error_detail =
                BuildWindowsErrorDetail("CreateFileW", response.normalized_path, error_value);
            return response;
        }

        response.writer = std::make_unique<WindowsDurableFileWriter>(
            file_handle, response.normalized_path);
        return response;
#endif
    }

    DurableFileResult WindowsDurableFile::PublishStagedFile(
        const PublishDurableFileRequest &request)
    {
        DurableFileResult result;
#ifndef _WIN32
        return UnsupportedPlatformResult("PublishStagedFile", "Windows");
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
                result.error = MapWindowsErrorToDurableFileErrorCode(
                    static_cast<DWORD>(create_ec.value()), true);
                result.error_detail = "create directories failed for " +
                                      PathToUtf8String(final_parent_path) + ": " +
                                      create_ec.message();
                return result;
            }
        }

        std::error_code exists_ec;
        if (request.mode == DurablePublishMode::kExclusive &&
            std::filesystem::exists(*final_path, exists_ec))
        {
            result.error = DurableFileErrorCode::kAtomicPublishFailed;
            result.error_detail = "final path already exists: " +
                                  PathToUtf8String(*final_path);
            return result;
        }

        const auto wide_staging_path = PrepareWindowsApiPath(*staging_path);
        const auto wide_final_path = PrepareWindowsApiPath(*final_path);
        if (!wide_staging_path.has_value() || !wide_final_path.has_value())
        {
            result.error = DurableFileErrorCode::kPathInvalid;
            result.error_detail = "unable to convert UTF-8 durable path to UTF-16";
            return result;
        }

        DWORD move_flags = MOVEFILE_WRITE_THROUGH;
        if (request.mode == DurablePublishMode::kReplaceExisting)
        {
            move_flags |= MOVEFILE_REPLACE_EXISTING;
        }

        if (::MoveFileExW(wide_staging_path->c_str(),
                          wide_final_path->c_str(),
                          move_flags) == 0)
        {
            const DWORD error_value = ::GetLastError();
            result.error = MapWindowsErrorToDurableFileErrorCode(error_value, false);
            if (error_value == ERROR_SHARING_VIOLATION ||
                error_value == ERROR_LOCK_VIOLATION ||
                error_value == ERROR_NOT_SAME_DEVICE ||
                error_value == ERROR_ALREADY_EXISTS ||
                error_value == ERROR_FILE_EXISTS)
            {
                result.error = DurableFileErrorCode::kAtomicPublishFailed;
            }
            result.error_detail =
                BuildWindowsErrorDetail("MoveFileExW", *final_path, error_value);
            return result;
        }

        result.durable_boundary_reached = true;
        return result;
#endif
    }

    DurableFileResult WindowsDurableFile::SyncDirectory(
        const SyncDurableDirectoryRequest &request)
    {
        DurableFileResult result;
#ifndef _WIN32
        return UnsupportedPlatformResult("SyncDirectory", "Windows");
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

        std::error_code exists_ec;
        if (!std::filesystem::exists(*directory_path, exists_ec) ||
            !std::filesystem::is_directory(*directory_path, exists_ec))
        {
            result.error = DurableFileErrorCode::kPathInvalid;
            result.error_detail = "directory path does not exist: " +
                                  PathToUtf8String(*directory_path);
            return result;
        }

        result.error = DurableFileErrorCode::kUnsupported;
        result.error_detail =
            "Windows directory durability is not implemented; explicit unsupported returned";
        return result;
#endif
    }

    const std::filesystem::path &WindowsDurableFile::root_path() const
    {
        return root_path_;
    }
}
