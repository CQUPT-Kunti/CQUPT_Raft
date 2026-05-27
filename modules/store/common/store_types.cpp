#include "store/common/store_types.h"

namespace raftdemo
{
    const char *ToString(const StorageNodeStatusCode code)
    {
        switch (code)
        {
        case StorageNodeStatusCode::kOk:
            return "Ok";
        case StorageNodeStatusCode::kAlreadyExists:
            return "AlreadyExists";
        case StorageNodeStatusCode::kNotFound:
            return "NotFound";
        case StorageNodeStatusCode::kConflict:
            return "Conflict";
        case StorageNodeStatusCode::kChecksumMismatch:
            return "ChecksumMismatch";
        case StorageNodeStatusCode::kCorrupted:
            return "Corrupted";
        case StorageNodeStatusCode::kDiskFull:
            return "DiskFull";
        case StorageNodeStatusCode::kPermissionDenied:
            return "PermissionDenied";
        case StorageNodeStatusCode::kIoError:
            return "IoError";
        case StorageNodeStatusCode::kTimeout:
            return "Timeout";
        case StorageNodeStatusCode::kCancelled:
            return "Cancelled";
        case StorageNodeStatusCode::kOverloaded:
            return "Overloaded";
        case StorageNodeStatusCode::kNodeUnavailable:
            return "NodeUnavailable";
        case StorageNodeStatusCode::kUnsupported:
            return "Unsupported";
        case StorageNodeStatusCode::kInvalidArgument:
            return "InvalidArgument";
        }

        return "Unknown";
    }

    const char *ToString(const ChunkState state)
    {
        switch (state)
        {
        case ChunkState::kStaging:
            return "Staging";
        case ChunkState::kLive:
            return "Live";
        case ChunkState::kDeleting:
            return "Deleting";
        case ChunkState::kDeleted:
            return "Deleted";
        case ChunkState::kQuarantined:
            return "Quarantined";
        case ChunkState::kCorrupted:
            return "Corrupted";
        case ChunkState::kMissing:
            return "Missing";
        }

        return "Unknown";
    }

    bool IsRetriableStatus(const StorageNodeStatusCode code)
    {
        switch (code)
        {
        case StorageNodeStatusCode::kTimeout:
        case StorageNodeStatusCode::kIoError:
        case StorageNodeStatusCode::kOverloaded:
        case StorageNodeStatusCode::kNodeUnavailable:
            return true;
        case StorageNodeStatusCode::kOk:
        case StorageNodeStatusCode::kAlreadyExists:
        case StorageNodeStatusCode::kNotFound:
        case StorageNodeStatusCode::kConflict:
        case StorageNodeStatusCode::kChecksumMismatch:
        case StorageNodeStatusCode::kCorrupted:
        case StorageNodeStatusCode::kDiskFull:
        case StorageNodeStatusCode::kPermissionDenied:
        case StorageNodeStatusCode::kCancelled:
        case StorageNodeStatusCode::kUnsupported:
        case StorageNodeStatusCode::kInvalidArgument:
        default:
            return false;
        }
    }

    bool IsReadableChunkState(const ChunkState state)
    {
        return state == ChunkState::kLive;
    }

    bool IsTerminalChunkState(const ChunkState state)
    {
        switch (state)
        {
        case ChunkState::kDeleted:
        case ChunkState::kQuarantined:
        case ChunkState::kCorrupted:
        case ChunkState::kMissing:
            return true;
        case ChunkState::kStaging:
        case ChunkState::kLive:
        case ChunkState::kDeleting:
        default:
            return false;
        }
    }

    bool ChunkLocation::IsValid() const
    {
        return !node_id.empty() && !chunk_id.empty();
    }

    bool ChunkChecksum::IsSet() const
    {
        return algorithm != ChunkChecksumAlgorithm::kUnknown &&
               !value.empty() &&
               size_bytes > 0;
    }

    bool ChunkIdentity::HasChunkKey() const
    {
        return !chunk_id.empty();
    }

    bool ChunkReplica::IsReadable() const
    {
        return !node_id.empty() &&
               !chunk_id.empty() &&
               size > 0 &&
               checksum.IsSet() &&
               IsReadableChunkState(state);
    }

    bool ChunkMetadata::IsReadable() const
    {
        return identity.HasChunkKey() &&
               size > 0 &&
               checksum.IsSet() &&
               IsReadableChunkState(state);
    }

    bool ChunkIndexEntry::HasFinalPath() const
    {
        return !final_path.empty();
    }

} // namespace raftdemo
