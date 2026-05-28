#include "store/common/store_types.h"

#include <charconv>
#include <cctype>
namespace storedemo
{
    namespace
    {
        void SetErrorDetail(std::string *error_detail, const std::string_view detail)
        {
            if (error_detail != nullptr)
            {
                error_detail->assign(detail);
            }
        }

        bool IsAllowedChunkObjectIdChar(const char ch)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            return std::isalnum(value) != 0 || ch == '-' || ch == '_' || ch == '.';
        }

        bool HasCanonicalUnsignedEncoding(const std::string_view text)
        {
            return text.size() == 1 || text.front() != '0';
        }

        template <typename UInt>
        bool ParseUnsigned(const std::string_view text, UInt *out_value)
        {
            if (out_value == nullptr || text.empty())
            {
                return false;
            }

            UInt value = 0;
            const char *begin = text.data();
            const char *end = begin + text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{} || ptr != end)
            {
                return false;
            }

            *out_value = value;
            return true;
        }
    } // namespace

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

    StorageNodeStatusCode ValidateChunkObjectId(const std::string_view object_id,
                                                std::string *error_detail)
    {
        if (object_id.empty())
        {
            SetErrorDetail(error_detail, "object_id must not be empty");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.size() > kMaxChunkObjectIdLength)
        {
            SetErrorDetail(error_detail, "object_id exceeds chunk filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id == "." || object_id == "..")
        {
            SetErrorDetail(error_detail, "object_id must not be relative path segment");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.find("..") != std::string_view::npos)
        {
            SetErrorDetail(error_detail, "object_id must not contain path escape marker");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.front() == '.' || object_id.back() == '.')
        {
            SetErrorDetail(error_detail,
                           "object_id must not start or end with '.'");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (object_id.find(kChunkIdSeparator) != std::string_view::npos)
        {
            SetErrorDetail(error_detail,
                           "object_id must not contain chunk id separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        for (const char ch : object_id)
        {
            if (!IsAllowedChunkObjectIdChar(ch))
            {
                SetErrorDetail(error_detail,
                               "object_id contains path separator or unsafe character");
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode MakeChunkId(const std::string_view object_id,
                                      const std::uint64_t version,
                                      const std::uint32_t chunk_index,
                                      ChunkId *out_chunk_id,
                                      std::string *error_detail)
    {
        if (out_chunk_id == nullptr)
        {
            SetErrorDetail(error_detail, "out_chunk_id must not be null");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const StorageNodeStatusCode object_id_status =
            ValidateChunkObjectId(object_id, error_detail);
        if (object_id_status != StorageNodeStatusCode::kOk)
        {
            return object_id_status;
        }

        if (version == 0)
        {
            SetErrorDetail(error_detail, "version must be greater than zero");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::string chunk_id = std::string(object_id) + kChunkIdSeparator +
                                     std::to_string(version) + kChunkIdSeparator +
                                     std::to_string(chunk_index);
        if (chunk_id.size() > kMaxChunkIdLength)
        {
            SetErrorDetail(error_detail, "chunk_id exceeds filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        *out_chunk_id = chunk_id;
        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode ParseChunkId(const std::string_view chunk_id,
                                       ChunkIdentity *out_identity,
                                       std::string *error_detail)
    {
        if (chunk_id.empty())
        {
            SetErrorDetail(error_detail, "chunk_id must not be empty");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (chunk_id.size() > kMaxChunkIdLength)
        {
            SetErrorDetail(error_detail, "chunk_id exceeds filename safe length");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::size_t first_separator = chunk_id.find(kChunkIdSeparator);
        if (first_separator == std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id missing first separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::size_t second_separator =
            chunk_id.find(kChunkIdSeparator, first_separator + 1U);
        if (second_separator == std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id missing second separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (chunk_id.find(kChunkIdSeparator, second_separator + 1U) !=
            std::string_view::npos)
        {
            SetErrorDetail(error_detail, "chunk_id has unexpected extra separator");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        const std::string_view object_id = chunk_id.substr(0, first_separator);
        const std::string_view version_text =
            chunk_id.substr(first_separator + 1U,
                            second_separator - first_separator - 1U);
        const std::string_view chunk_index_text =
            chunk_id.substr(second_separator + 1U);

        const StorageNodeStatusCode object_id_status =
            ValidateChunkObjectId(object_id, error_detail);
        if (object_id_status != StorageNodeStatusCode::kOk)
        {
            return object_id_status;
        }

        if (version_text.empty() || chunk_index_text.empty())
        {
            SetErrorDetail(error_detail, "chunk_id must contain non-empty version and chunk_index");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (!HasCanonicalUnsignedEncoding(version_text))
        {
            SetErrorDetail(error_detail, "version must use canonical unsigned encoding");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (!HasCanonicalUnsignedEncoding(chunk_index_text))
        {
            SetErrorDetail(error_detail, "chunk_index must use canonical unsigned encoding");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        std::uint64_t version = 0;
        if (!ParseUnsigned(version_text, &version))
        {
            SetErrorDetail(error_detail, "version is not a valid uint64");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (version == 0)
        {
            SetErrorDetail(error_detail, "version must be greater than zero");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        std::uint32_t chunk_index = 0;
        if (!ParseUnsigned(chunk_index_text, &chunk_index))
        {
            SetErrorDetail(error_detail, "chunk_index is not a valid uint32");
            return StorageNodeStatusCode::kInvalidArgument;
        }

        if (out_identity != nullptr)
        {
            out_identity->chunk_id = std::string(chunk_id);
            out_identity->object_id = std::string(object_id);
            out_identity->version = version;
            out_identity->chunk_index = chunk_index;
            out_identity->offset = 0;
        }

        return StorageNodeStatusCode::kOk;
    }

    StorageNodeStatusCode ValidateChunkId(const std::string_view chunk_id,
                                          std::string *error_detail)
    {
        return ParseChunkId(chunk_id, nullptr, error_detail);
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

} // namespace storedemo
