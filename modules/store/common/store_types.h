#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace storedemo
{
    using StorageNodeId = std::string;
    using ChunkId = std::string;

    enum class StorageNodeStatusCode : std::uint8_t
    {
        kOk = 0,
        kAlreadyExists = 1,
        kNotFound = 2,
        kConflict = 3,
        kChecksumMismatch = 4,
        kCorrupted = 5,
        kDiskFull = 6,
        kPermissionDenied = 7,
        kIoError = 8,
        kTimeout = 9,
        kCancelled = 10,
        kOverloaded = 11,
        kNodeUnavailable = 12,
        kUnsupported = 13,
        kInvalidArgument = 14,
    };

    enum class ChunkState : std::uint8_t
    {
        kStaging = 0,
        kLive = 1,
        kDeleting = 2,
        kDeleted = 3,
        kQuarantined = 4,
        kCorrupted = 5,
        kMissing = 6,
    };

    enum class ChunkChecksumAlgorithm : std::uint8_t
    {
        kUnknown = 0,
        kSha256 = 1,
    };

    const char *ToString(StorageNodeStatusCode code);
    const char *ToString(ChunkState state);
    bool IsRetriableStatus(StorageNodeStatusCode code);
    bool IsReadableChunkState(ChunkState state);
    bool IsTerminalChunkState(ChunkState state);

    struct ChunkIdentity;

    inline constexpr char kChunkIdSeparator = '~';
    inline constexpr std::size_t kMaxChunkIdLength = 255;
    inline constexpr std::size_t kMaxChunkObjectIdLength = 223;

    StorageNodeStatusCode ValidateChunkObjectId(std::string_view object_id,
                                                std::string *error_detail = nullptr);

    StorageNodeStatusCode MakeChunkId(std::string_view object_id,
                                      std::uint64_t version,
                                      std::uint32_t chunk_index,
                                      ChunkId *out_chunk_id,
                                      std::string *error_detail = nullptr);

    StorageNodeStatusCode ParseChunkId(std::string_view chunk_id,
                                       ChunkIdentity *out_identity,
                                       std::string *error_detail = nullptr);

    StorageNodeStatusCode ValidateChunkId(std::string_view chunk_id,
                                          std::string *error_detail = nullptr);

    struct ChunkLocation
    {
        StorageNodeId node_id;
        ChunkId chunk_id;

        bool IsValid() const;
    };

    struct ChunkChecksum
    {
        ChunkChecksumAlgorithm algorithm{ChunkChecksumAlgorithm::kUnknown};
        std::string value;
        std::uint64_t size_bytes{0};
        std::uint64_t computed_at{0};

        bool IsSet() const;
    };

    inline constexpr std::size_t kSha256DigestBytes = 32;
    inline constexpr std::size_t kSha256DigestHexChars = 64;

    StorageNodeStatusCode ComputeChunkChecksum(std::string_view payload,
                                               ChunkChecksum *out_checksum,
                                               std::string *error_detail = nullptr);

    StorageNodeStatusCode VerifyChunkChecksum(std::string_view payload,
                                              const ChunkChecksum &expected_checksum,
                                              ChunkChecksum *out_actual_checksum = nullptr,
                                              std::string *error_detail = nullptr);

    struct ChunkIdentity
    {
        ChunkId chunk_id;
        std::string object_id;
        std::uint64_t version{0};
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};

        bool HasChunkKey() const;
    };

    struct ChunkReplica
    {
        ChunkId chunk_id;
        StorageNodeId node_id;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        ChunkState state{ChunkState::kMissing};
        std::uint64_t last_verified_at{0};
        std::uint64_t last_read_at{0};
        std::uint32_t failure_count{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};

        bool IsReadable() const;
    };

    struct ChunkMetadata
    {
        ChunkIdentity identity;
        StorageNodeId node_id;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        ChunkState state{ChunkState::kMissing};
        std::string write_request_id;
        std::string delete_request_id;
        std::uint64_t created_at{0};
        std::uint64_t published_at{0};
        std::uint64_t deleted_at{0};
        std::uint64_t last_verified_at{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string quarantine_reason;

        bool IsReadable() const;
    };

    struct ChunkIndexEntry
    {
        ChunkIdentity identity;
        ChunkState state{ChunkState::kMissing};
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::filesystem::path final_path;
        std::filesystem::path staging_path;
        std::filesystem::path metadata_path;
        std::size_t lock_shard{0};
        std::uint64_t updated_at{0};

        bool HasFinalPath() const;
    };

    enum class StoreModuleStage : std::uint8_t
    {
        kPlaceholder = 0,
    };

}
