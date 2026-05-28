#include "store/io/durable_file.h"

namespace storedemo
{
    namespace
    {
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
}
