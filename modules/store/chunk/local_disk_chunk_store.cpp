#include "store/chunk/local_disk_chunk_store.h"

#include <algorithm>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <map>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"

namespace storedemo
{
    namespace
    {
        constexpr const char *kChunksDirectoryRelativePath = "chunks";
        constexpr const char *kLiveDirectoryRelativePath = "chunks/live";
        constexpr const char *kStagingDirectoryRelativePath = "chunks/staging";
        constexpr const char *kQuarantineDirectoryRelativePath = "chunks/quarantine";

        std::uint64_t CurrentUnixTimeMillis()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        }

        std::string BuildHexToken(const std::string_view input)
        {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const unsigned char ch : input)
            {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= 1099511628211ULL;
            }

            std::ostringstream stream;
            stream << "rq" << std::hex << std::setfill('0') << std::setw(16) << hash;
            return stream.str();
        }

        bool HasExpectedChecksumConstraint(const ChunkChecksum &checksum)
        {
            return checksum.algorithm != ChunkChecksumAlgorithm::kUnknown ||
                   !checksum.value.empty() ||
                   checksum.size_bytes != 0 ||
                   checksum.computed_at != 0;
        }

        bool HasRequiredDurableBoundary(const DurableFileResult &result)
        {
            return result.ok() && result.durable_boundary_reached;
        }

        StorageNodeStatusCode MapFilesystemErrorToStatus(const std::error_code &error)
        {
            if (!error)
            {
                return StorageNodeStatusCode::kOk;
            }

            if (error == std::errc::no_space_on_device ||
                error == std::errc::file_too_large)
            {
                return StorageNodeStatusCode::kDiskFull;
            }

            if (error == std::errc::permission_denied ||
                error == std::errc::operation_not_permitted ||
                error == std::errc::read_only_file_system)
            {
                return StorageNodeStatusCode::kPermissionDenied;
            }

            if (error == std::errc::invalid_argument ||
                error == std::errc::no_such_file_or_directory ||
                error == std::errc::not_a_directory ||
                error == std::errc::filename_too_long ||
                error == std::errc::too_many_symbolic_link_levels)
            {
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kIoError;
        }

        std::string BuildFilesystemErrorDetail(const char *operation,
                                               const std::filesystem::path &path,
                                               const std::error_code &error)
        {
            return std::string(operation) + " failed for " + path.string() +
                   ": " + error.message();
        }

        StorageNodeStatusCode EnsureDirectoryExists(const std::filesystem::path &path,
                                                    std::string *error_detail)
        {
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(path, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        BuildFilesystemErrorDetail("exists", path, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }

            if (exists)
            {
                std::error_code directory_error;
                const bool is_directory =
                    std::filesystem::is_directory(path, directory_error);
                if (directory_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = BuildFilesystemErrorDetail(
                            "is_directory", path, directory_error);
                    }
                    return MapFilesystemErrorToStatus(directory_error);
                }

                if (!is_directory)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "path exists but is not a directory: " +
                                        path.string();
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                return StorageNodeStatusCode::kOk;
            }

            std::error_code create_error;
            std::filesystem::create_directories(path, create_error);
            if (create_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "create_directories", path, create_error);
                }
                return MapFilesystemErrorToStatus(create_error);
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveStorePath(const std::filesystem::path &data_root,
                                               const std::filesystem::path &relative_path,
                                               std::filesystem::path *resolved_path,
                                               std::string *error_detail)
        {
            return ResolveDurablePathUnderRoot(data_root,
                                               relative_path,
                                               resolved_path,
                                               error_detail);
        }

        std::shared_ptr<DurableFile> CreateDefaultDurableFile(
            const std::filesystem::path &data_root)
        {
#if defined(__linux__)
            return std::make_shared<LinuxDurableFile>(data_root);
#elif defined(_WIN32)
            return std::make_shared<WindowsDurableFile>(data_root);
#else
            return {};
#endif
        }

        ChunkMetadata BuildChunkMetadata(const LocalDiskChunkStoreConfig &config,
                                         const ChunkIdentity &identity,
                                         const std::uint64_t size,
                                         const ChunkChecksum &checksum,
                                         const ChunkState state,
                                         const std::string &write_request_id,
                                         const std::uint64_t timestamp_ms)
        {
            ChunkMetadata metadata;
            metadata.identity = identity;
            metadata.node_id = config.node_id;
            metadata.size = size;
            metadata.checksum = checksum;
            metadata.state = state;
            metadata.write_request_id = write_request_id;
            metadata.created_at = timestamp_ms;
            metadata.published_at = timestamp_ms;
            metadata.last_error = StorageNodeStatusCode::kOk;
            return metadata;
        }

        ChunkMetadata BuildChunkMetadataFromIndexEntry(
            const LocalDiskChunkStoreConfig &config,
            const ChunkIndexEntry &entry,
            const std::string &write_request_id)
        {
            ChunkMetadata metadata;
            metadata.identity = entry.identity;
            metadata.node_id = config.node_id;
            metadata.size = entry.size;
            metadata.checksum = entry.checksum;
            metadata.state = entry.state;
            metadata.write_request_id = write_request_id;
            metadata.created_at = entry.updated_at;
            metadata.published_at = entry.updated_at;
            metadata.last_error = StorageNodeStatusCode::kOk;
            return metadata;
        }

        ChunkIndexEntry BuildChunkIndexEntry(const ChunkMetadata &metadata,
                                             const std::filesystem::path &final_path,
                                             const std::uint64_t timestamp_ms)
        {
            ChunkIndexEntry entry;
            entry.identity = metadata.identity;
            entry.state = metadata.state;
            entry.size = metadata.size;
            entry.checksum = metadata.checksum;
            entry.final_path = final_path;
            entry.updated_at = timestamp_ms;
            return entry;
        }

        StorageNodeStatusCode ResolveIndexedFinalPath(const std::filesystem::path &data_root,
                                                      const ChunkIndexEntry &entry,
                                                      std::filesystem::path *final_path,
                                                      std::string *error_detail)
        {
            if (final_path == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "indexed final_path output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (entry.HasFinalPath())
            {
                if (entry.final_path.is_absolute())
                {
                    *final_path = entry.final_path.lexically_normal();
                    return StorageNodeStatusCode::kOk;
                }

                return ResolveStorePath(data_root, entry.final_path, final_path, error_detail);
            }

            ChunkPathLayout layout;
            const auto layout_status = BuildChunkPathLayout(entry.identity.chunk_id,
                                                            "read-probe",
                                                            &layout,
                                                            error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                return layout_status;
            }

            return ResolveStorePath(data_root,
                                    layout.final_relative_path,
                                    final_path,
                                    error_detail);
        }

        StorageNodeStatusCode CompareChecksums(const ChunkChecksum &expected_checksum,
                                               const ChunkChecksum &actual_checksum,
                                               std::string *error_detail)
        {
            if (expected_checksum.algorithm == ChunkChecksumAlgorithm::kUnknown)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum algorithm must be set";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (expected_checksum.algorithm != actual_checksum.algorithm)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "checksum algorithm mismatch";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            if (expected_checksum.size_bytes != actual_checksum.size_bytes ||
                expected_checksum.value != actual_checksum.value)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "payload checksum mismatch";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateReadableChunkState(const ChunkState state,
                                                         std::string *error_detail)
        {
            if (IsReadableChunkState(state))
            {
                return StorageNodeStatusCode::kOk;
            }

            if (error_detail != nullptr)
            {
                *error_detail = std::string("chunk state is not readable: ") +
                                ToString(state);
            }

            switch (state)
            {
            case ChunkState::kCorrupted:
            case ChunkState::kQuarantined:
                return StorageNodeStatusCode::kCorrupted;
            case ChunkState::kDeleted:
            case ChunkState::kMissing:
                return StorageNodeStatusCode::kNotFound;
            case ChunkState::kStaging:
            case ChunkState::kDeleting:
            case ChunkState::kLive:
            default:
                return StorageNodeStatusCode::kConflict;
            }
        }

        StorageNodeStatusCode ValidateExpectedReadChecksum(const ChunkChecksum &expected_checksum,
                                                           const ChunkChecksum &actual_checksum,
                                                           std::string *error_detail)
        {
            if (!HasExpectedChecksumConstraint(expected_checksum))
            {
                return StorageNodeStatusCode::kOk;
            }

            if (expected_checksum.algorithm != ChunkChecksumAlgorithm::kSha256)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kUnsupported;
            }

            if (expected_checksum.value.size() != kSha256DigestHexChars)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum value must be 64 hex chars";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return CompareChecksums(expected_checksum, actual_checksum, error_detail);
        }

        StorageNodeStatusCode ReadFilePayload(const std::filesystem::path &path,
                                              std::string *payload,
                                              std::string *error_detail)
        {
            if (payload == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "payload output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed to open chunk file for read: " +
                                    path.string();
                }
                return StorageNodeStatusCode::kIoError;
            }

            payload->assign(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
            if (!input.good() && !input.eof())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed while reading chunk file: " +
                                    path.string();
                }
                return StorageNodeStatusCode::kIoError;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveEntryChecksum(const std::filesystem::path &data_root,
                                                   const ChunkIndexEntry &entry,
                                                   ChunkChecksum *checksum,
                                                   std::string *error_detail)
        {
            if (checksum == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "checksum output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (entry.checksum.IsSet())
            {
                *checksum = entry.checksum;
                return StorageNodeStatusCode::kOk;
            }

            std::filesystem::path final_path;
            auto status =
                ResolveIndexedFinalPath(data_root, entry, &final_path, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            std::error_code exists_error;
            const bool exists = std::filesystem::exists(final_path, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        BuildFilesystemErrorDetail("exists", final_path, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }
            if (!exists)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "final chunk file does not exist: " +
                                    final_path.string();
                }
                return StorageNodeStatusCode::kNotFound;
            }

            std::string payload;
            status = ReadFilePayload(final_path, &payload, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return ComputeChunkChecksum(payload, checksum, error_detail);
        }

        StorageNodeStatusCode ValidateDeleteExpectedChecksum(
            const std::filesystem::path &data_root,
            const ChunkIndexEntry &entry,
            const ChunkChecksum &expected_checksum,
            std::string *error_detail)
        {
            if (!HasExpectedChecksumConstraint(expected_checksum))
            {
                return StorageNodeStatusCode::kOk;
            }

            if (expected_checksum.algorithm != ChunkChecksumAlgorithm::kSha256)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kUnsupported;
            }

            if (expected_checksum.value.size() != kSha256DigestHexChars)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum value must be 64 hex chars";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkChecksum actual_checksum;
            const auto status =
                ResolveEntryChecksum(data_root, entry, &actual_checksum, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return CompareChecksums(expected_checksum, actual_checksum, error_detail);
        }

        StorageNodeStatusCode PrepareWriteIdentity(const WriteChunkRequest &request,
                                                   ChunkIdentity *out_identity,
                                                   std::string *error_detail)
        {
            if (out_identity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "write identity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.chunk_id.empty())
            {
                if (request.identity.object_id.empty() || request.identity.version == 0)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "write request must include chunk_id or full chunk identity";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                ChunkId chunk_id;
                const auto make_status = MakeChunkId(request.identity.object_id,
                                                     request.identity.version,
                                                     request.identity.chunk_index,
                                                     &chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }

                out_identity->chunk_id = std::move(chunk_id);
                out_identity->object_id = request.identity.object_id;
                out_identity->version = request.identity.version;
                out_identity->chunk_index = request.identity.chunk_index;
                out_identity->offset = request.identity.offset;
                return StorageNodeStatusCode::kOk;
            }

            ChunkIdentity parsed_identity;
            const auto parse_status =
                ParseChunkId(request.identity.chunk_id, &parsed_identity, error_detail);
            if (parse_status != StorageNodeStatusCode::kOk)
            {
                return parse_status;
            }

            if (!request.identity.object_id.empty() &&
                request.identity.object_id != parsed_identity.object_id)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match object_id";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.version != 0 &&
                request.identity.version != parsed_identity.version)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match version";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.identity.chunk_index != 0 &&
                request.identity.chunk_index != parsed_identity.chunk_index)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id does not match chunk_index";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            parsed_identity.offset = request.identity.offset;
            *out_identity = std::move(parsed_identity);
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode CollectRegularFileCandidatePaths(
            const std::filesystem::path &data_root,
            const std::filesystem::path &scan_root,
            const char *root_label,
            std::vector<std::filesystem::path> *relative_paths,
            std::string *error_detail)
        {
            if (relative_paths == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = std::string(root_label) +
                                    " candidate output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            relative_paths->clear();

            std::error_code exists_error;
            const bool exists = std::filesystem::exists(scan_root, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        BuildFilesystemErrorDetail("exists", scan_root, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }
            if (!exists)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = std::string(root_label) + " root does not exist: " +
                                    scan_root.string();
                }
                return StorageNodeStatusCode::kNotFound;
            }

            std::error_code directory_error;
            const bool is_directory =
                std::filesystem::is_directory(scan_root, directory_error);
            if (directory_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "is_directory", scan_root, directory_error);
                }
                return MapFilesystemErrorToStatus(directory_error);
            }
            if (!is_directory)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = std::string(root_label) +
                                    " root is not a directory: " +
                                    scan_root.string();
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::error_code iter_error;
            std::filesystem::recursive_directory_iterator iter(scan_root, iter_error);
            if (iter_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = std::string("failed to iterate ") + root_label +
                                    " root: " + iter_error.message();
                }
                return StorageNodeStatusCode::kIoError;
            }

            for (const auto end = std::filesystem::recursive_directory_iterator();
                 iter != end;
                 iter.increment(iter_error))
            {
                if (iter_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = std::string("failed while scanning ") +
                                        root_label + " root: " + iter_error.message();
                    }
                    return StorageNodeStatusCode::kIoError;
                }

                std::error_code status_error;
                const bool is_regular = iter->is_regular_file(status_error);
                if (status_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = std::string("failed to inspect ") +
                                        root_label + " candidate type: " +
                                        status_error.message();
                    }
                    return StorageNodeStatusCode::kIoError;
                }

                const auto relative_path =
                    iter->path().lexically_relative(data_root).lexically_normal();
                if (!is_regular)
                {
                    continue;
                }

                relative_paths->push_back(relative_path);
            }

            std::sort(relative_paths->begin(), relative_paths->end());
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode CollectLiveChunkCandidatePaths(
            const std::filesystem::path &data_root,
            const std::filesystem::path &live_root,
            std::vector<std::filesystem::path> *relative_paths,
            std::string *error_detail)
        {
            return CollectRegularFileCandidatePaths(data_root,
                                                    live_root,
                                                    "live chunk",
                                                    relative_paths,
                                                    error_detail);
        }

        StorageNodeStatusCode CollectQuarantineChunkCandidatePaths(
            const std::filesystem::path &data_root,
            const std::filesystem::path &quarantine_root,
            std::vector<std::filesystem::path> *relative_paths,
            std::string *error_detail)
        {
            return CollectRegularFileCandidatePaths(data_root,
                                                    quarantine_root,
                                                    "quarantine chunk",
                                                    relative_paths,
                                                    error_detail);
        }

        StorageNodeStatusCode ParseChunkIdFromLiveFilename(
            const std::filesystem::path &relative_path,
            ChunkId *chunk_id,
            std::string *error_detail)
        {
            if (chunk_id == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *chunk_id = relative_path.stem().string();
            return ValidateChunkId(*chunk_id, error_detail);
        }

        StorageNodeStatusCode IsCanonicalLiveChunkPath(const ChunkId &chunk_id,
                                                       const std::filesystem::path &relative_path,
                                                       bool *is_canonical,
                                                       std::string *error_detail)
        {
            if (is_canonical == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "canonical path output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *is_canonical = false;

            ChunkPathLayout layout;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, "rebuild", &layout, error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                return layout_status;
            }

            *is_canonical = relative_path == layout.final_relative_path;
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode BuildQuarantineChunkRelativePath(
            const ChunkId &chunk_id,
            std::filesystem::path *relative_path,
            std::string *error_detail)
        {
            if (relative_path == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "quarantine relative path output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkPathLayout layout;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, "quarantine", &layout, error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                return layout_status;
            }

            const auto relative_under_live =
                layout.final_relative_path.lexically_relative(
                    std::filesystem::path(kLiveDirectoryRelativePath));
            if (relative_under_live.empty() || relative_under_live == ".")
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "failed to derive quarantine path relative to live root";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *relative_path =
                (std::filesystem::path(kQuarantineDirectoryRelativePath) /
                 relative_under_live)
                    .lexically_normal();
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode IsCanonicalQuarantineChunkPath(
            const ChunkId &chunk_id,
            const std::filesystem::path &relative_path,
            bool *is_canonical,
            std::string *error_detail)
        {
            if (is_canonical == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "canonical path output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *is_canonical = false;

            std::filesystem::path expected_relative_path;
            const auto status =
                BuildQuarantineChunkRelativePath(chunk_id,
                                                 &expected_relative_path,
                                                 error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            *is_canonical = relative_path == expected_relative_path;
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode RecoverFinalChunkPayloadFacts(
            const std::filesystem::path &final_path,
            std::uint64_t *size,
            ChunkChecksum *checksum,
            std::string *error_detail)
        {
            if (size == nullptr || checksum == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "size/checksum outputs must not be null during rebuild";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::string payload;
            auto status = ReadFilePayload(final_path, &payload, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            *size = static_cast<std::uint64_t>(payload.size());
            status = ComputeChunkChecksum(payload, checksum, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return StorageNodeStatusCode::kOk;
        }

        ChunkIndexEntry BuildRebuiltChunkIndexEntry(
            const ChunkIdentity &identity,
            const std::uint64_t size,
            const ChunkChecksum &checksum,
            const ChunkState state,
            const std::filesystem::path &final_relative_path)
        {
            ChunkIndexEntry entry;
            entry.identity = identity;
            entry.state = state;
            entry.size = size;
            entry.checksum = checksum;
            entry.final_path = final_relative_path;
            entry.updated_at = 0;
            return entry;
        }

        StorageNodeStatusCode QuarantineChunkEntry(
            const LocalDiskChunkStorePaths &paths,
            ChunkIndex *chunk_index,
            const ChunkId &chunk_id,
            ChunkIndexEntry *quarantined_entry,
            std::string *error_detail)
        {
            if (!paths.IsInitialized())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "quarantine requires initialized store paths";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (chunk_index == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "quarantine requires a valid ChunkIndex";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            auto lock_response = chunk_index->AcquireChunkLock(chunk_id);
            if (!lock_response.ok() || !lock_response.acquired)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = lock_response.error_detail;
                }
                return lock_response.status;
            }

            const auto find_response = chunk_index->Find(chunk_id);
            if (!find_response.ok())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = find_response.error_detail;
                }
                return find_response.status;
            }

            ChunkIndexEntry entry = find_response.entry;
            if (entry.state == ChunkState::kQuarantined ||
                entry.state == ChunkState::kCorrupted)
            {
                if (quarantined_entry != nullptr)
                {
                    *quarantined_entry = entry;
                }
                return StorageNodeStatusCode::kOk;
            }

            if (entry.state != ChunkState::kLive)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = std::string("cannot quarantine non-LIVE chunk state: ") +
                                    ToString(entry.state);
                }
                return StorageNodeStatusCode::kConflict;
            }

            std::filesystem::path source_path;
            auto status = ResolveIndexedFinalPath(paths.data_root,
                                                  entry,
                                                  &source_path,
                                                  error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            std::error_code source_exists_error;
            const bool source_exists =
                std::filesystem::exists(source_path, source_exists_error);
            if (source_exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail("exists",
                                                               source_path,
                                                               source_exists_error);
                }
                return MapFilesystemErrorToStatus(source_exists_error);
            }
            if (!source_exists)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "final chunk file does not exist: " +
                                    source_path.string();
                }
                return StorageNodeStatusCode::kNotFound;
            }

            std::filesystem::path quarantine_relative_path;
            status = BuildQuarantineChunkRelativePath(chunk_id,
                                                      &quarantine_relative_path,
                                                      error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            std::filesystem::path quarantine_path;
            status = ResolveStorePath(paths.data_root,
                                      quarantine_relative_path,
                                      &quarantine_path,
                                      error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = EnsureDirectoryExists(quarantine_path.parent_path(), error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            if (source_path.lexically_normal() != quarantine_path.lexically_normal())
            {
                std::error_code target_exists_error;
                const bool target_exists =
                    std::filesystem::exists(quarantine_path, target_exists_error);
                if (target_exists_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = BuildFilesystemErrorDetail("exists",
                                                                   quarantine_path,
                                                                   target_exists_error);
                    }
                    return MapFilesystemErrorToStatus(target_exists_error);
                }
                if (target_exists)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "quarantine target already exists: " +
                                        quarantine_path.string();
                    }
                    return StorageNodeStatusCode::kConflict;
                }

                std::error_code rename_error;
                std::filesystem::rename(source_path, quarantine_path, rename_error);
                if (rename_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = BuildFilesystemErrorDetail("rename",
                                                                   source_path,
                                                                   rename_error);
                    }
                    if (rename_error == std::errc::no_such_file_or_directory)
                    {
                        return StorageNodeStatusCode::kNotFound;
                    }
                    return MapFilesystemErrorToStatus(rename_error);
                }
            }

            entry.state = ChunkState::kQuarantined;
            entry.final_path = quarantine_relative_path;
            entry.updated_at = CurrentUnixTimeMillis();

            const auto update_response = chunk_index->Update(entry);
            if (!update_response.ok())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = update_response.error_detail;
                }
                return update_response.status;
            }

            if (quarantined_entry != nullptr)
            {
                *quarantined_entry = update_response.entry;
            }
            return StorageNodeStatusCode::kOk;
        }

        struct StagingCleanupScanResult
        {
            std::vector<std::filesystem::path> file_candidates;
            std::vector<std::filesystem::path> directory_candidates;
        };

        StorageNodeStatusCode CollectStagingCleanupCandidates(
            const std::filesystem::path &staging_root,
            StagingCleanupScanResult *scan_result,
            std::string *error_detail)
        {
            if (scan_result == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "staging cleanup scan output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *scan_result = {};

            std::error_code exists_error;
            const bool exists = std::filesystem::exists(staging_root, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "exists", staging_root, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }
            if (!exists)
            {
                return StorageNodeStatusCode::kOk;
            }

            std::error_code iter_error;
            std::filesystem::recursive_directory_iterator iter(staging_root, iter_error);
            if (iter_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed to iterate staging root: " +
                                    iter_error.message();
                }
                return StorageNodeStatusCode::kIoError;
            }

            for (const auto end = std::filesystem::recursive_directory_iterator();
                 iter != end;
                 iter.increment(iter_error))
            {
                if (iter_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "failed while scanning staging root: " +
                                        iter_error.message();
                    }
                    return StorageNodeStatusCode::kIoError;
                }

                std::error_code status_error;
                const bool is_directory = iter->is_directory(status_error);
                if (status_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "failed to inspect staging candidate directory type: " +
                            status_error.message();
                    }
                    return StorageNodeStatusCode::kIoError;
                }

                if (is_directory)
                {
                    scan_result->directory_candidates.push_back(iter->path());
                    continue;
                }

                const bool is_regular = iter->is_regular_file(status_error);
                if (status_error)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "failed to inspect staging candidate file type: " +
                            status_error.message();
                    }
                    return StorageNodeStatusCode::kIoError;
                }

                if (is_regular)
                {
                    scan_result->file_candidates.push_back(iter->path());
                }
            }

            std::sort(scan_result->file_candidates.begin(),
                      scan_result->file_candidates.end());
            std::sort(scan_result->directory_candidates.begin(),
                      scan_result->directory_candidates.end(),
                      [](const std::filesystem::path &lhs,
                         const std::filesystem::path &rhs)
                      {
                          const auto lhs_depth =
                              std::distance(lhs.begin(), lhs.end());
                          const auto rhs_depth =
                              std::distance(rhs.begin(), rhs.end());
                          if (lhs_depth != rhs_depth)
                          {
                              return lhs_depth > rhs_depth;
                          }
                          return lhs < rhs;
                      });

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode IsStagingCandidatePastGracePeriod(
            const std::filesystem::path &path,
            const std::uint64_t grace_period_ms,
            bool *is_stale,
            std::string *error_detail)
        {
            if (is_stale == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "staging staleness output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *is_stale = false;
            if (grace_period_ms == 0)
            {
                return StorageNodeStatusCode::kOk;
            }

            std::error_code write_time_error;
            const auto last_write_time =
                std::filesystem::last_write_time(path, write_time_error);
            if (write_time_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "last_write_time", path, write_time_error);
                }
                return MapFilesystemErrorToStatus(write_time_error);
            }

            const auto now = std::filesystem::file_time_type::clock::now();
            if (last_write_time > now)
            {
                return StorageNodeStatusCode::kOk;
            }

            const auto age =
                std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                      last_write_time);
            *is_stale = age >= std::chrono::milliseconds(grace_period_ms);
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode RemovePathSafelyUnderRoot(
            const std::filesystem::path &root_path,
            const std::filesystem::path &target_path,
            std::string *error_detail)
        {
            const auto normalized_root = root_path.lexically_normal();
            const auto normalized_target = target_path.lexically_normal();
            const auto relative = normalized_target.lexically_relative(normalized_root);
            if (relative.empty() || relative == "." ||
                relative.native().starts_with(".."))
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "refusing to remove path outside staging root: " +
                                    normalized_target.string();
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::error_code exists_error;
            const bool exists = std::filesystem::exists(normalized_target, exists_error);
            if (exists_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "exists", normalized_target, exists_error);
                }
                return MapFilesystemErrorToStatus(exists_error);
            }
            if (!exists)
            {
                return StorageNodeStatusCode::kOk;
            }

            std::error_code remove_error;
            const bool removed = std::filesystem::remove(normalized_target, remove_error);
            if (remove_error)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = BuildFilesystemErrorDetail(
                        "remove", normalized_target, remove_error);
                }
                return MapFilesystemErrorToStatus(remove_error);
            }

            if (!removed)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed to remove staging path: " +
                                    normalized_target.string();
                }
                return StorageNodeStatusCode::kIoError;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode CleanupStaleStagingFiles(
            const std::filesystem::path &staging_root,
            const std::uint64_t grace_period_ms,
            const StagingCleanupScanResult &scan_result,
            std::string *error_detail)
        {
            for (const auto &path : scan_result.file_candidates)
            {
                bool is_stale = false;
                auto status = IsStagingCandidatePastGracePeriod(path,
                                                                grace_period_ms,
                                                                &is_stale,
                                                                error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return status;
                }

                if (!is_stale)
                {
                    continue;
                }

                status = RemovePathSafelyUnderRoot(staging_root, path, error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return status;
                }
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode PruneEmptyStagingDirectories(
            const std::filesystem::path &staging_root,
            const StagingCleanupScanResult &scan_result,
            std::string *error_detail)
        {
            for (const auto &directory_path : scan_result.directory_candidates)
            {
                std::error_code empty_error;
                const bool is_empty =
                    std::filesystem::is_empty(directory_path, empty_error);
                if (empty_error)
                {
                    if (empty_error == std::errc::no_such_file_or_directory)
                    {
                        continue;
                    }

                    if (error_detail != nullptr)
                    {
                        *error_detail = BuildFilesystemErrorDetail(
                            "is_empty", directory_path, empty_error);
                    }
                    return MapFilesystemErrorToStatus(empty_error);
                }

                if (!is_empty)
                {
                    continue;
                }

                const auto status =
                    RemovePathSafelyUnderRoot(staging_root, directory_path, error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return status;
                }
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode CleanupStaleStagingArtifacts(
            const LocalDiskChunkStorePaths &paths,
            const std::uint64_t grace_period_ms,
            std::string *error_detail)
        {
            if (!paths.IsInitialized())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "staging cleanup requires initialized store paths";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (grace_period_ms == 0)
            {
                return StorageNodeStatusCode::kOk;
            }

            StagingCleanupScanResult scan_result;
            auto status = CollectStagingCleanupCandidates(paths.staging_root,
                                                          &scan_result,
                                                          error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = CleanupStaleStagingFiles(paths.staging_root,
                                              grace_period_ms,
                                              scan_result,
                                              error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return PruneEmptyStagingDirectories(paths.staging_root,
                                                scan_result,
                                                error_detail);
        }

        StorageNodeStatusCode ClearChunkIndexEntries(ChunkIndex *chunk_index,
                                                     std::string *error_detail)
        {
            if (chunk_index == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_index must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::string page_token;
            while (true)
            {
                auto list_response = chunk_index->List(ChunkIndexListOptions{
                    .page_token = page_token,
                    .page_size = std::numeric_limits<std::size_t>::max()});
                if (!list_response.ok())
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = list_response.error_detail;
                    }
                    return list_response.status;
                }

                for (const auto &entry : list_response.entries)
                {
                    const auto remove_response =
                        chunk_index->Remove(entry.identity.chunk_id);
                    if (!remove_response.ok() &&
                        remove_response.status != StorageNodeStatusCode::kNotFound)
                    {
                        if (error_detail != nullptr)
                        {
                            *error_detail = remove_response.error_detail;
                        }
                        return remove_response.status;
                    }
                }

                if (list_response.next_page_token.empty())
                {
                    break;
                }

                page_token = list_response.next_page_token;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode InsertRecoveredChunkIndexEntry(
            ChunkIndex *chunk_index,
            const ChunkIndexEntry &entry,
            std::string *error_detail)
        {
            if (chunk_index == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_index must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            const auto insert_response = chunk_index->Insert(entry);
            if (!insert_response.ok())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = insert_response.error_detail;
                }
                return insert_response.status;
            }

            return StorageNodeStatusCode::kOk;
        }

        template <typename Response>
        Response MakeUnsupportedResponse(const char *operation)
        {
            Response response;
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail =
                std::string("LocalDiskChunkStore::") + operation +
                " is not implemented in the current LocalDiskChunkStore stage";
            return response;
        }
    }

    bool LocalDiskChunkStorePaths::IsInitialized() const
    {
        return !data_root.empty() &&
               !chunks_root.empty() &&
               !live_root.empty() &&
               !staging_root.empty() &&
               !quarantine_root.empty();
    }

    LocalDiskChunkStore::LocalDiskChunkStore(LocalDiskChunkStoreConfig config)
        : config_(std::move(config))
    {
    }

    LocalDiskChunkStore::~LocalDiskChunkStore() = default;

    LocalDiskChunkStoreInitResult LocalDiskChunkStore::Initialize()
    {
        LocalDiskChunkStoreInitResult result;
        if (initialized_)
        {
            result.paths = paths_;
            result.initialized = true;
            return result;
        }

        if (config_.node_id.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore node_id must not be empty";
            return result;
        }

        if (config_.data_dir.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore data_dir must not be empty";
            return result;
        }

        LocalDiskChunkStorePaths candidate_paths;
        std::error_code absolute_error;
        candidate_paths.data_root =
            std::filesystem::absolute(config_.data_dir, absolute_error).lexically_normal();
        if (absolute_error)
        {
            result.status = MapFilesystemErrorToStatus(absolute_error);
            result.error_detail = BuildFilesystemErrorDetail(
                "absolute", config_.data_dir, absolute_error);
            return result;
        }
        if (candidate_paths.data_root.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "LocalDiskChunkStore data_dir resolved to empty path";
            return result;
        }

        auto status = ResolveStorePath(candidate_paths.data_root,
                                       kChunksDirectoryRelativePath,
                                       &candidate_paths.chunks_root,
                                       &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = ResolveStorePath(candidate_paths.data_root,
                                  kLiveDirectoryRelativePath,
                                  &candidate_paths.live_root,
                                  &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = ResolveStorePath(candidate_paths.data_root,
                                  kStagingDirectoryRelativePath,
                                  &candidate_paths.staging_root,
                                  &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = ResolveStorePath(candidate_paths.data_root,
                                  kQuarantineDirectoryRelativePath,
                                  &candidate_paths.quarantine_root,
                                  &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        if (config_.chunk_index == nullptr)
        {
            config_.chunk_index = std::make_shared<ShardedChunkIndex>();
        }

        if (config_.durable_file == nullptr)
        {
            config_.durable_file = CreateDefaultDurableFile(candidate_paths.data_root);
            if (config_.durable_file == nullptr)
            {
                result.status = StorageNodeStatusCode::kUnsupported;
                result.error_detail =
                    "LocalDiskChunkStore has no default durable file implementation for this platform";
                return result;
            }
        }

        status = EnsureDirectoryExists(candidate_paths.data_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.chunks_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.live_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.staging_root, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        status = EnsureDirectoryExists(candidate_paths.quarantine_root,
                                       &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.status = status;
            return result;
        }

        paths_ = std::move(candidate_paths);
        const auto staging_cleanup_status =
            CleanupStaleStagingArtifacts(paths_,
                                         config_.staging_cleanup_grace_period_ms,
                                         &result.error_detail);
        if (staging_cleanup_status != StorageNodeStatusCode::kOk)
        {
            result.status = staging_cleanup_status;
            paths_ = {};
            return result;
        }

        const auto rebuild_result = RebuildIndexFromDisk();
        if (!rebuild_result.ok())
        {
            result.status = rebuild_result.status;
            result.error_detail = rebuild_result.error_detail;
            result.retry_after_ms = rebuild_result.retry_after_ms;
            paths_ = {};
            return result;
        }

        initialized_ = true;

        result.paths = paths_;
        result.initialized = true;
        return result;
    }

    ChunkStoreResult LocalDiskChunkStore::RebuildIndexFromDisk()
    {
        ChunkStoreResult result;

        if (!paths_.IsInitialized())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail =
                "RebuildIndexFromDisk requires initialized store paths";
            return result;
        }

        if (config_.chunk_index == nullptr)
        {
            result.status = StorageNodeStatusCode::kUnsupported;
            result.error_detail = "RebuildIndexFromDisk requires a valid ChunkIndex";
            return result;
        }

        std::vector<std::filesystem::path> live_candidate_paths;
        result.status = CollectLiveChunkCandidatePaths(paths_.data_root,
                                                       paths_.live_root,
                                                       &live_candidate_paths,
                                                       &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::vector<std::filesystem::path> quarantine_candidate_paths;
        result.status = CollectQuarantineChunkCandidatePaths(paths_.data_root,
                                                             paths_.quarantine_root,
                                                             &quarantine_candidate_paths,
                                                             &result.error_detail);
        if (result.status != StorageNodeStatusCode::kOk &&
            result.status != StorageNodeStatusCode::kNotFound)
        {
            return result;
        }

        struct RebuildCandidate
        {
            std::filesystem::path relative_path;
            ChunkState state{ChunkState::kMissing};
        };

        std::map<ChunkId, std::vector<RebuildCandidate>> candidates_by_chunk_id;
        for (const auto &relative_path : live_candidate_paths)
        {
            if (relative_path.extension() != ".chunk")
            {
                continue;
            }

            ChunkId chunk_id;
            std::string parse_error;
            if (ParseChunkIdFromLiveFilename(relative_path, &chunk_id, &parse_error) !=
                StorageNodeStatusCode::kOk)
            {
                continue;
            }

            candidates_by_chunk_id[chunk_id].push_back(RebuildCandidate{
                .relative_path = relative_path,
                .state = ChunkState::kLive});
        }

        for (const auto &relative_path : quarantine_candidate_paths)
        {
            if (relative_path.extension() != ".chunk")
            {
                continue;
            }

            ChunkId chunk_id;
            std::string parse_error;
            if (ParseChunkIdFromLiveFilename(relative_path, &chunk_id, &parse_error) !=
                StorageNodeStatusCode::kOk)
            {
                continue;
            }

            candidates_by_chunk_id[chunk_id].push_back(RebuildCandidate{
                .relative_path = relative_path,
                .state = ChunkState::kQuarantined});
        }

        for (const auto &[chunk_id, candidates] : candidates_by_chunk_id)
        {
            if (candidates.size() > 1U)
            {
                result.status = StorageNodeStatusCode::kConflict;
                result.error_detail =
                    "duplicate rebuild chunk candidates found for chunk_id " +
                    chunk_id;
                return result;
            }
        }

        std::vector<ChunkIndexEntry> recovered_entries;
        recovered_entries.reserve(candidates_by_chunk_id.size());
        for (const auto &[chunk_id, candidates] : candidates_by_chunk_id)
        {
            const auto &candidate = candidates.front();
            bool is_canonical = false;
            if (candidate.state == ChunkState::kLive)
            {
                result.status = IsCanonicalLiveChunkPath(chunk_id,
                                                         candidate.relative_path,
                                                         &is_canonical,
                                                         &result.error_detail);
            }
            else
            {
                result.status = IsCanonicalQuarantineChunkPath(chunk_id,
                                                               candidate.relative_path,
                                                               &is_canonical,
                                                               &result.error_detail);
            }
            if (!result.ok())
            {
                return result;
            }
            if (!is_canonical)
            {
                continue;
            }

            std::filesystem::path final_path;
            result.status = ResolveStorePath(paths_.data_root,
                                             candidate.relative_path,
                                             &final_path,
                                             &result.error_detail);
            if (!result.ok())
            {
                return result;
            }

            std::uint64_t size = 0;
            ChunkChecksum checksum;
            result.status = RecoverFinalChunkPayloadFacts(final_path,
                                                          &size,
                                                          &checksum,
                                                          &result.error_detail);
            if (!result.ok())
            {
                return result;
            }

            ChunkIdentity identity;
            result.status =
                ParseChunkId(chunk_id, &identity, &result.error_detail);
            if (!result.ok())
            {
                return result;
            }

            recovered_entries.push_back(BuildRebuiltChunkIndexEntry(identity,
                                                                    size,
                                                                    checksum,
                                                                    candidate.state,
                                                                    candidate.relative_path));
        }

        result.status =
            ClearChunkIndexEntries(config_.chunk_index.get(), &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        for (const auto &entry : recovered_entries)
        {
            result.status = InsertRecoveredChunkIndexEntry(config_.chunk_index.get(),
                                                           entry,
                                                           &result.error_detail);
            if (!result.ok())
            {
                return result;
            }
        }

        result.status = StorageNodeStatusCode::kOk;
        return result;
    }

    const LocalDiskChunkStoreConfig &LocalDiskChunkStore::config() const
    {
        return config_;
    }

    const LocalDiskChunkStorePaths &LocalDiskChunkStore::paths() const
    {
        return paths_;
    }

    bool LocalDiskChunkStore::initialized() const
    {
        return initialized_;
    }

    DurableFile *LocalDiskChunkStore::durable_file() const
    {
        return config_.durable_file.get();
    }

    ChunkIndex *LocalDiskChunkStore::chunk_index() const
    {
        return config_.chunk_index.get();
    }

    BoundedStorageExecutor *LocalDiskChunkStore::executor() const
    {
        return config_.executor.get();
    }

    WriteChunkResponse LocalDiskChunkStore::WriteChunk(const WriteChunkRequest &request)
    {
        WriteChunkResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "WriteChunk request_id must not be empty";
            return response;
        }

        ChunkIdentity identity;
        response.status = PrepareWriteIdentity(request, &identity, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (request.expected_size.has_value() &&
            *request.expected_size != static_cast<std::uint64_t>(request.payload.size()))
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "WriteChunk payload size does not match expected_size";
            return response;
        }

        ChunkChecksum actual_checksum;
        if (HasExpectedChecksumConstraint(request.expected_checksum))
        {
            response.status = VerifyChunkChecksum(request.payload,
                                                  request.expected_checksum,
                                                  &actual_checksum,
                                                  &response.error_detail);
        }
        else
        {
            response.status = ComputeChunkChecksum(request.payload,
                                                   &actual_checksum,
                                                   &response.error_detail);
        }
        if (!response.ok())
        {
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "WriteChunk requires a valid ChunkIndex";
            return response;
        }

        if (config_.durable_file == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "WriteChunk requires a valid DurableFile";
            return response;
        }

        auto lock_response = config_.chunk_index->AcquireChunkLock(identity.chunk_id);
        if (!lock_response.ok() || !lock_response.acquired)
        {
            response.status = lock_response.status;
            response.error_detail = lock_response.error_detail;
            response.retry_after_ms = lock_response.retry_after_ms;
            return response;
        }

        const auto existing_entry = config_.chunk_index->Find(identity.chunk_id);
        if (existing_entry.ok() && existing_entry.found)
        {
            response.metadata =
                BuildChunkMetadataFromIndexEntry(config_, existing_entry.entry, request.request_id);
            if (existing_entry.entry.state == ChunkState::kLive &&
                existing_entry.entry.size ==
                    static_cast<std::uint64_t>(request.payload.size()) &&
                existing_entry.entry.checksum.algorithm == actual_checksum.algorithm &&
                existing_entry.entry.checksum.value == actual_checksum.value &&
                existing_entry.entry.checksum.size_bytes == actual_checksum.size_bytes)
            {
                response.status = StorageNodeStatusCode::kOk;
                response.durable = true;
                response.already_exists = true;
                return response;
            }

            response.status = StorageNodeStatusCode::kConflict;
            response.error_detail =
                "WriteChunk conflicts with existing local chunk content or state";
            return response;
        }

        if (existing_entry.status != StorageNodeStatusCode::kNotFound)
        {
            response.status = existing_entry.status;
            response.error_detail = existing_entry.error_detail;
            response.retry_after_ms = existing_entry.retry_after_ms;
            return response;
        }

        ChunkPathLayout layout;
        response.status = BuildChunkPathLayout(identity.chunk_id,
                                               BuildHexToken(request.request_id),
                                               &layout,
                                               &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        std::filesystem::path final_path;
        response.status = ResolveStorePath(paths_.data_root,
                                           layout.final_relative_path,
                                           &final_path,
                                           &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        auto open_response = config_.durable_file->OpenStagingWriter(OpenStagingWriterRequest{
            .relative_path = layout.staging_relative_path,
            .expected_size = static_cast<std::uint64_t>(request.payload.size()),
            .context = {}});
        if (!open_response.ok())
        {
            response.status = open_response.status_code();
            response.error_detail = open_response.error_detail;
            response.retry_after_ms = open_response.retry_after_ms;
            return response;
        }

        if (open_response.writer == nullptr)
        {
            response.status = StorageNodeStatusCode::kIoError;
            response.error_detail = "OpenStagingWriter succeeded without returning a writer";
            return response;
        }

        const auto payload_bytes =
            std::as_bytes(std::span(request.payload.data(), request.payload.size()));
        const auto append_result = open_response.writer->Append(DurableAppendRequest{
            .buffer = payload_bytes,
            .context = {}});
        if (!append_result.ok())
        {
            response.status = append_result.status_code();
            response.error_detail = append_result.error_detail;
            response.retry_after_ms = append_result.retry_after_ms;
            return response;
        }

        const auto flush_result = open_response.writer->Flush(DurableFlushRequest{
            .mode = DurableFlushMode::kDataOnly,
            .context = {}});
        if (!HasRequiredDurableBoundary(flush_result))
        {
            response.status = flush_result.ok() ? StorageNodeStatusCode::kIoError
                                                : flush_result.status_code();
            response.error_detail = flush_result.ok()
                                        ? "WriteChunk flush did not reach required durable boundary"
                                        : flush_result.error_detail;
            response.retry_after_ms = flush_result.retry_after_ms;
            return response;
        }

        const auto close_result = open_response.writer->Close(DurableCloseRequest{});
        if (!close_result.ok())
        {
            response.status = close_result.status_code();
            response.error_detail = close_result.error_detail;
            response.retry_after_ms = close_result.retry_after_ms;
            return response;
        }

        const auto publish_result = config_.durable_file->PublishStagedFile(
            PublishDurableFileRequest{
                .staging_path = layout.staging_relative_path,
                .final_path = layout.final_relative_path,
                .mode = DurablePublishMode::kExclusive,
                .context = {}});
        if (!HasRequiredDurableBoundary(publish_result))
        {
            response.status = publish_result.ok() ? StorageNodeStatusCode::kIoError
                                                  : publish_result.status_code();
            response.error_detail = publish_result.ok()
                                        ? "WriteChunk publish did not reach required durable boundary"
                                        : publish_result.error_detail;
            response.retry_after_ms = publish_result.retry_after_ms;
            return response;
        }

        const auto sync_result = config_.durable_file->SyncDirectory(
            SyncDurableDirectoryRequest{
                .directory_path = layout.final_relative_path.parent_path(),
                .context = {}});
        if (!HasRequiredDurableBoundary(sync_result))
        {
            response.status = sync_result.ok() ? StorageNodeStatusCode::kIoError
                                               : sync_result.status_code();
            response.error_detail = sync_result.ok()
                                        ? "WriteChunk directory sync did not reach required durable boundary"
                                        : sync_result.error_detail;
            response.retry_after_ms = sync_result.retry_after_ms;
            return response;
        }

        const std::uint64_t timestamp_ms = CurrentUnixTimeMillis();
        response.metadata = BuildChunkMetadata(config_,
                                              identity,
                                              static_cast<std::uint64_t>(request.payload.size()),
                                              actual_checksum,
                                              ChunkState::kLive,
                                              request.request_id,
                                              timestamp_ms);
        ChunkIndexEntry new_entry =
            BuildChunkIndexEntry(response.metadata, final_path, timestamp_ms);

        const auto insert_response = config_.chunk_index->Insert(new_entry);
        if (!insert_response.ok())
        {
            if (insert_response.status == StorageNodeStatusCode::kAlreadyExists &&
                insert_response.entry.state == ChunkState::kLive &&
                insert_response.entry.size == new_entry.size &&
                insert_response.entry.checksum.algorithm == new_entry.checksum.algorithm &&
                insert_response.entry.checksum.value == new_entry.checksum.value &&
                insert_response.entry.checksum.size_bytes == new_entry.checksum.size_bytes)
            {
                response.metadata = BuildChunkMetadataFromIndexEntry(
                    config_, insert_response.entry, request.request_id);
                response.status = StorageNodeStatusCode::kOk;
                response.durable = true;
                response.already_exists = true;
                return response;
            }

            response.status = insert_response.status;
            response.error_detail = insert_response.error_detail;
            response.retry_after_ms = insert_response.retry_after_ms;
            return response;
        }

        response.metadata.identity = insert_response.entry.identity;
        response.metadata.size = insert_response.entry.size;
        response.metadata.checksum = insert_response.entry.checksum;
        response.metadata.state = insert_response.entry.state;
        response.durable = true;
        return response;
    }

    ReadChunkResponse LocalDiskChunkStore::ReadChunk(const ReadChunkRequest &request)
    {
        ReadChunkResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "ReadChunk request_id must not be empty";
            return response;
        }

        if (request.range.has_value())
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail =
                "LocalDiskChunkStore::ReadChunk range reads are not implemented in the current stage";
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "ReadChunk requires a valid ChunkIndex";
            return response;
        }

        const auto find_response = config_.chunk_index->Find(request.chunk_id);
        if (!find_response.ok())
        {
            response.status = find_response.status;
            response.error_detail = find_response.error_detail;
            response.retry_after_ms = find_response.retry_after_ms;
            return response;
        }

        const auto &entry = find_response.entry;
        response.metadata = BuildChunkMetadataFromIndexEntry(config_, entry, "");

        response.status =
            ValidateReadableChunkState(entry.state, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        std::filesystem::path final_path;
        response.status =
            ResolveIndexedFinalPath(paths_.data_root, entry, &final_path, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        std::error_code exists_error;
        const bool exists = std::filesystem::exists(final_path, exists_error);
        if (exists_error)
        {
            response.status = MapFilesystemErrorToStatus(exists_error);
            response.error_detail =
                BuildFilesystemErrorDetail("exists", final_path, exists_error);
            return response;
        }
        if (!exists)
        {
            response.status = StorageNodeStatusCode::kNotFound;
            response.error_detail = "final chunk file does not exist: " +
                                    final_path.string();
            return response;
        }

        std::string payload;
        response.status = ReadFilePayload(final_path, &payload, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (static_cast<std::uint64_t>(payload.size()) != entry.size)
        {
            response.status = QuarantineChunkEntry(paths_,
                                                   config_.chunk_index.get(),
                                                   entry.identity.chunk_id,
                                                   nullptr,
                                                   &response.error_detail);
            if (response.status == StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kCorrupted;
                response.error_detail =
                    "chunk file size does not match index metadata";
            }
            return response;
        }

        ChunkChecksum actual_checksum;
        response.status =
            ComputeChunkChecksum(payload, &actual_checksum, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }
        response.actual_checksum = actual_checksum;

        if (entry.checksum.IsSet() &&
            (entry.checksum.algorithm != actual_checksum.algorithm ||
             entry.checksum.size_bytes != actual_checksum.size_bytes ||
             entry.checksum.value != actual_checksum.value))
        {
            response.status = QuarantineChunkEntry(paths_,
                                                   config_.chunk_index.get(),
                                                   entry.identity.chunk_id,
                                                   nullptr,
                                                   &response.error_detail);
            if (response.status == StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kCorrupted;
                response.error_detail =
                    "chunk file checksum does not match index metadata";
            }
            return response;
        }

        response.status = ValidateExpectedReadChecksum(request.expected_checksum,
                                                       actual_checksum,
                                                       &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        response.metadata.checksum =
            entry.checksum.IsSet() ? entry.checksum : actual_checksum;
        response.payload = std::move(payload);
        response.verified = request.verify_checksum || entry.checksum.IsSet() ||
                            HasExpectedChecksumConstraint(request.expected_checksum);
        return response;
    }

    DeleteChunkResponse LocalDiskChunkStore::DeleteChunk(const DeleteChunkRequest &request)
    {
        DeleteChunkResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "DeleteChunk request_id must not be empty";
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "DeleteChunk requires a valid ChunkIndex";
            return response;
        }

        auto lock_response = config_.chunk_index->AcquireChunkLock(request.chunk_id);
        if (!lock_response.ok() || !lock_response.acquired)
        {
            response.status = lock_response.status;
            response.error_detail = lock_response.error_detail;
            response.retry_after_ms = lock_response.retry_after_ms;
            return response;
        }

        const auto find_response = config_.chunk_index->Find(request.chunk_id);
        if (find_response.status == StorageNodeStatusCode::kNotFound)
        {
            response.status = StorageNodeStatusCode::kOk;
            response.already_missing = true;
            response.metadata.identity.chunk_id = request.chunk_id;
            response.metadata.state = ChunkState::kMissing;
            return response;
        }
        if (!find_response.ok())
        {
            response.status = find_response.status;
            response.error_detail = find_response.error_detail;
            response.retry_after_ms = find_response.retry_after_ms;
            return response;
        }

        ChunkIndexEntry updated_entry = find_response.entry;
        response.metadata = BuildChunkMetadataFromIndexEntry(config_,
                                                            updated_entry,
                                                            request.request_id);

        if (updated_entry.state == ChunkState::kDeleted ||
            updated_entry.state == ChunkState::kMissing)
        {
            response.status = StorageNodeStatusCode::kOk;
            response.already_missing = true;
            response.metadata.state = ChunkState::kDeleted;
            return response;
        }

        response.status = ValidateDeleteExpectedChecksum(paths_.data_root,
                                                         updated_entry,
                                                         request.expected_checksum,
                                                         &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (updated_entry.HasFinalPath())
        {
            std::filesystem::path final_path;
            response.status = ResolveIndexedFinalPath(paths_.data_root,
                                                      updated_entry,
                                                      &final_path,
                                                      &response.error_detail);
            if (!response.ok())
            {
                return response;
            }

            std::error_code remove_error;
            const bool removed = std::filesystem::remove(final_path, remove_error);
            if (remove_error)
            {
                response.status = MapFilesystemErrorToStatus(remove_error);
                response.error_detail =
                    BuildFilesystemErrorDetail("remove", final_path, remove_error);
                return response;
            }
            (void)removed;
        }

        updated_entry.state = ChunkState::kDeleted;
        updated_entry.updated_at = CurrentUnixTimeMillis();
        const auto update_response = config_.chunk_index->Update(updated_entry);
        if (!update_response.ok())
        {
            response.status = update_response.status;
            response.error_detail = update_response.error_detail;
            response.retry_after_ms = update_response.retry_after_ms;
            return response;
        }

        response.metadata = BuildChunkMetadataFromIndexEntry(config_,
                                                            update_response.entry,
                                                            request.request_id);
        response.deleted = true;
        return response;
    }

    StatChunkResponse LocalDiskChunkStore::StatChunk(const StatChunkRequest &request)
    {
        StatChunkResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "StatChunk request_id must not be empty";
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "StatChunk requires a valid ChunkIndex";
            return response;
        }

        const auto find_response = config_.chunk_index->Find(request.chunk_id);
        if (!find_response.ok())
        {
            response.status = find_response.status;
            response.error_detail = find_response.error_detail;
            response.retry_after_ms = find_response.retry_after_ms;
            return response;
        }

        response.metadata =
            BuildChunkMetadataFromIndexEntry(config_, find_response.entry, "");
        if (!request.verify_checksum)
        {
            return response;
        }

        if (!IsReadableChunkState(find_response.entry.state))
        {
            response.status = StorageNodeStatusCode::kConflict;
            response.error_detail =
                std::string("cannot verify checksum for non-LIVE chunk state: ") +
                ToString(find_response.entry.state);
            return response;
        }

        std::filesystem::path final_path;
        response.status = ResolveIndexedFinalPath(paths_.data_root,
                                                  find_response.entry,
                                                  &final_path,
                                                  &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        std::error_code exists_error;
        const bool exists = std::filesystem::exists(final_path, exists_error);
        if (exists_error)
        {
            response.status = MapFilesystemErrorToStatus(exists_error);
            response.error_detail =
                BuildFilesystemErrorDetail("exists", final_path, exists_error);
            return response;
        }
        if (!exists)
        {
            response.status = StorageNodeStatusCode::kNotFound;
            response.error_detail = "final chunk file does not exist: " +
                                    final_path.string();
            return response;
        }

        std::string payload;
        response.status = ReadFilePayload(final_path, &payload, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (static_cast<std::uint64_t>(payload.size()) != find_response.entry.size)
        {
            response.status = QuarantineChunkEntry(paths_,
                                                   config_.chunk_index.get(),
                                                   find_response.entry.identity.chunk_id,
                                                   nullptr,
                                                   &response.error_detail);
            if (response.status == StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kCorrupted;
                response.error_detail =
                    "chunk file size does not match index metadata";
            }
            return response;
        }

        ChunkChecksum actual_checksum;
        response.status =
            ComputeChunkChecksum(payload, &actual_checksum, &response.error_detail);
        if (!response.ok())
        {
            return response;
        }

        if (find_response.entry.checksum.IsSet())
        {
            response.status = CompareChecksums(find_response.entry.checksum,
                                               actual_checksum,
                                               &response.error_detail);
            if (!response.ok())
            {
                response.status = QuarantineChunkEntry(paths_,
                                                       config_.chunk_index.get(),
                                                       find_response.entry.identity.chunk_id,
                                                       nullptr,
                                                       &response.error_detail);
                if (response.status == StorageNodeStatusCode::kOk)
                {
                    response.status = StorageNodeStatusCode::kCorrupted;
                }
                return response;
            }
        }

        response.metadata.checksum =
            find_response.entry.checksum.IsSet() ? find_response.entry.checksum
                                                 : actual_checksum;
        response.verified = true;
        return response;
    }

    ListChunksResponse LocalDiskChunkStore::ListChunks(const ListChunksRequest &request)
    {
        ListChunksResponse response;

        if (!initialized_)
        {
            const auto init_result = Initialize();
            if (!init_result.ok())
            {
                response.status = init_result.status;
                response.error_detail = init_result.error_detail;
                response.retry_after_ms = init_result.retry_after_ms;
                return response;
            }
        }

        if (request.request_id.empty())
        {
            response.status = StorageNodeStatusCode::kInvalidArgument;
            response.error_detail = "ListChunks request_id must not be empty";
            return response;
        }

        if (config_.chunk_index == nullptr)
        {
            response.status = StorageNodeStatusCode::kUnsupported;
            response.error_detail = "ListChunks requires a valid ChunkIndex";
            return response;
        }

        ChunkIndexListOptions options;
        options.state_filter = request.options.state_filter;
        options.prefix_filter = request.options.prefix_filter;
        options.page_token = request.options.page_token;
        options.page_size = request.options.page_size;
        options.include_quarantine = request.options.include_quarantine;

        const auto list_response = config_.chunk_index->List(options);
        if (!list_response.ok())
        {
            response.status = list_response.status;
            response.error_detail = list_response.error_detail;
            response.retry_after_ms = list_response.retry_after_ms;
            return response;
        }

        response.next_page_token = list_response.next_page_token;
        response.snapshot_epoch = list_response.snapshot_epoch;
        response.chunks.reserve(list_response.entries.size());
        for (const auto &entry : list_response.entries)
        {
            response.chunks.push_back(BuildChunkMetadataFromIndexEntry(config_, entry, ""));
        }
        return response;
    }
}
